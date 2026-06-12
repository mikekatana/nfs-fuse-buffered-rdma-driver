#define FUSE_USE_VERSION 31
#define _GNU_SOURCE

#include <fuse.h>
#include <nfsc/libnfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <semaphore.h>
#include <sys/statvfs.h>
#include <signal.h>
#include <sys/mman.h>
#include <getopt.h> 
#include <sys/time.h>

// --- Configuration ---
#define NUM_CONSUMERS 96 
#define CHUNK_SIZE (1024 * 1024)    
#define POOL_CHUNKS 2048            
#define RING_SIZE 65536 

// --- Enums & Globals ---
typedef enum {
    PROTO_TCP,
    PROTO_RDMA
} transport_mode_t;

static transport_mode_t current_proto = PROTO_TCP; 
static char *memory_pool = NULL; 
static char *global_server = NULL;
static char *global_path = NULL;

// Contexts
static struct nfs_context *worker_ctx[NUM_CONSUMERS];
static struct nfs_context *main_ctx = NULL;
static pthread_mutex_t main_ctx_lock = PTHREAD_MUTEX_INITIALIZER;

// Flow Control
static sem_t sem_free_slots;
static sem_t sem_tasks_ready;
static atomic_int pending_write_count = 0; 

typedef struct {
    int slot_id;
    size_t size;
    off_t offset;
    uint64_t fh_ptr; 
} write_task_t;

struct ring_queue {
    write_task_t tasks[RING_SIZE];
    atomic_size_t head;
    atomic_size_t tail;
};

static struct ring_queue ring;
static atomic_bool running = true;
static pthread_t consumers[NUM_CONSUMERS];

// --- Memory Management ---
static void* alloc_standard(size_t size) {
    void *ptr;
    if (posix_memalign(&ptr, 4096, size) != 0) return NULL;
    memset(ptr, 0, size);
    return ptr;
}

static void* alloc_rdma_pinned(size_t size) {
    void *ptr = NULL;
    ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, 
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_LOCKED, -1, 0);

    if (ptr == MAP_FAILED) {
        fprintf(stderr, "[RDMA] Huge Pages failed. Fallback to aligned alloc.\n");
        if (posix_memalign(&ptr, 4096, size) != 0) return NULL;
    } else {
        fprintf(stderr, "[RDMA] Using Huge Pages (2MB) for Zero-Copy.\n");
    }

    if (mlock(ptr, size) != 0) perror("[RDMA] WARNING: mlock failed");
    
    memset(ptr, 0, size);
    return ptr;
}

// --- Helper: Ring Ops ---
static void push_task(write_task_t task) {
    size_t head = atomic_fetch_add(&ring.head, 1);
    ring.tasks[head % RING_SIZE] = task;
    sem_post(&sem_tasks_ready);
}

// --- Helper: Reconnection Logic ---
static void ensure_connected(int id) {
    while (worker_ctx[id] == NULL) {
        struct nfs_context *ctx = nfs_init_context();
        if (ctx == NULL) { sleep(1); continue; }

        if (nfs_mount(ctx, global_server, global_path) != 0) {
            nfs_destroy_context(ctx);
            sleep(1);
            continue;
        }

        if (current_proto == PROTO_RDMA) {
            nfs_set_readahead(ctx, 64 * 1024 * 1024);
        } else {
            nfs_set_readahead(ctx, 16 * 1024 * 1024);
        }
        
        worker_ctx[id] = ctx;
    }
}

// --- Consumer Thread ---
void *consumer_thread(void *arg) {
    int id = *(int*)arg;
    ensure_connected(id);
    
    while (running) {
        sem_wait(&sem_tasks_ready);
        if (!running) break;

        size_t tail = atomic_fetch_add(&ring.tail, 1);
        write_task_t task = ring.tasks[tail % RING_SIZE];
        char *data_ptr = memory_pool + (task.slot_id * CHUNK_SIZE);
        
        while (true) {
            ensure_connected(id);
            int ret = nfs_pwrite(worker_ctx[id], (struct nfsfh *)task.fh_ptr, task.offset, task.size, data_ptr);
            if (ret >= 0) break; 
            
            fprintf(stderr, "Worker %d: Write Failed (%s). Reconnecting...\n", id, nfs_get_error(worker_ctx[id]));
            nfs_destroy_context(worker_ctx[id]);
            worker_ctx[id] = NULL;
            usleep(100000); 
        }

        sem_post(&sem_free_slots);
        atomic_fetch_sub(&pending_write_count, 1);
    }
    return NULL;
}

// --- FUSE Init ---
static void *nfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    conn->want |= FUSE_CAP_WRITEBACK_CACHE;
    conn->want |= FUSE_CAP_ASYNC_READ;
    conn->max_background = 128;
    
    size_t total_mem = (size_t)POOL_CHUNKS * CHUNK_SIZE;
    
    if (current_proto == PROTO_RDMA) {
        memory_pool = (char*)alloc_rdma_pinned(total_mem);
        conn->max_readahead = 64 * 1024 * 1024;
    } else {
        memory_pool = (char*)alloc_standard(total_mem);
        conn->max_readahead = 16 * 1024 * 1024;
    }

    if (!memory_pool) abort();

    sem_init(&sem_free_slots, 0, POOL_CHUNKS);
    sem_init(&sem_tasks_ready, 0, 0);
    atomic_init(&pending_write_count, 0);

    static int ids[NUM_CONSUMERS];
    for (int i = 0; i < NUM_CONSUMERS; i++) {
        ids[i] = i;
        pthread_create(&consumers[i], NULL, consumer_thread, &ids[i]);
    }
    
    printf("NFS Driver Initialized. Mode: %s\n", (current_proto == PROTO_RDMA) ? "RDMA" : "TCP");
    return NULL;
}

// --- FUSE Operations ---

static int nfs_write_fuse(const char *path, const char *buf, size_t size,
                          off_t offset, struct fuse_file_info *fi) {
    size_t bytes_written = 0;
    static atomic_size_t slot_ticker = 0; 

    while (bytes_written < size) {
        size_t chunk = size - bytes_written;
        if (chunk > CHUNK_SIZE) chunk = CHUNK_SIZE;

        sem_wait(&sem_free_slots);

        size_t ticket = atomic_fetch_add(&slot_ticker, 1);
        int slot = ticket % POOL_CHUNKS;

        memcpy(memory_pool + (slot * CHUNK_SIZE), buf + bytes_written, chunk);
        atomic_fetch_add(&pending_write_count, 1);

        write_task_t task = {
            .slot_id = slot,
            .size = chunk,
            .offset = offset + bytes_written,
            .fh_ptr = fi->fh
        };
        push_task(task);
        bytes_written += chunk;
    }
    return size;
}

// --- ROBUST GETATTR (FIXES FIO EIO ERROR) ---
static int nfs_getattr_fuse(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    struct nfs_stat_64 nst;
    int ret = -1;

    pthread_mutex_lock(&main_ctx_lock);

    // 1. Try Handle (Fastest)
    if (fi && fi->fh) {
        ret = nfs_fstat64(main_ctx, (struct nfsfh *)fi->fh, &nst);
    }

    // 2. Fallback to Path (Safest)
    if (ret < 0) {
        ret = nfs_stat64(main_ctx, path, &nst);
    }
    
    int err = errno;
    pthread_mutex_unlock(&main_ctx_lock);
    
    // 3. Safety Check: 
    // If errno is 0, it means 'File Not Found', so return ENOENT.
    // Returning EIO here kills FIO during file creation.
    if (ret < 0) return (err == 0) ? -ENOENT : -err;
    
    stbuf->st_mode = nst.nfs_mode; 
    stbuf->st_nlink = nst.nfs_nlink;
    stbuf->st_size = nst.nfs_size; 
    stbuf->st_uid = nst.nfs_uid;
    stbuf->st_gid = nst.nfs_gid; 
    stbuf->st_atime = nst.nfs_atime;
    stbuf->st_mtime = nst.nfs_mtime; 
    stbuf->st_ctime = nst.nfs_ctime;
    
    // 4. Force 1MB Blocksize for RDMA Alignment
    stbuf->st_blksize = 1024 * 1024;
    stbuf->st_blocks = (stbuf->st_size + 511) / 512;

    return 0;
}

static int nfs_access_fuse(const char *path, int mask) {
    pthread_mutex_lock(&main_ctx_lock);
    int ret = nfs_access(main_ctx, path, mask);
    int err = errno;
    pthread_mutex_unlock(&main_ctx_lock);
    return (ret < 0) ? -err : 0;
}

static int nfs_mkdir_fuse(const char *path, mode_t mode) {
    pthread_mutex_lock(&main_ctx_lock);
    int ret = nfs_mkdir(main_ctx, path);
    int err = errno;
    pthread_mutex_unlock(&main_ctx_lock);
    return (ret < 0) ? -err : 0;
}

static int nfs_unlink_fuse(const char *path) {
    pthread_mutex_lock(&main_ctx_lock);
    int ret = nfs_unlink(main_ctx, path);
    int err = errno;
    pthread_mutex_unlock(&main_ctx_lock);
    return (ret < 0) ? -err : 0;
}

static int nfs_rmdir_fuse(const char *path) {
    pthread_mutex_lock(&main_ctx_lock);
    int ret = nfs_rmdir(main_ctx, path);
    int err = errno;
    pthread_mutex_unlock(&main_ctx_lock);
    return (ret < 0) ? -err : 0;
}

static int nfs_rename_fuse(const char *from, const char *to, unsigned int flags) {
    if (flags) return -EINVAL; 
    pthread_mutex_lock(&main_ctx_lock);
    int ret = nfs_rename(main_ctx, from, to);
    int err = errno;
    pthread_mutex_unlock(&main_ctx_lock);
    return (ret < 0) ? -err : 0;
}

static int nfs_open_fuse(const char *path, struct fuse_file_info *fi) {
    struct nfsfh *fh;
    pthread_mutex_lock(&main_ctx_lock);
    int ret = nfs_open(main_ctx, path, fi->flags, &fh);
    int err = errno;
    pthread_mutex_unlock(&main_ctx_lock);
    
    // Handle Open failures gracefully
    if (ret < 0) return (err == 0) ? -EIO : -err;
    
    fi->fh = (uint64_t)fh;
    fi->keep_cache = 1;
    return 0;
}

static int nfs_create_fuse(const char *path, mode_t mode, struct fuse_file_info *fi) {
    struct nfsfh *fh;
    pthread_mutex_lock(&main_ctx_lock);
    int ret = nfs_creat(main_ctx, path, mode, &fh);
    int err = errno;
    pthread_mutex_unlock(&main_ctx_lock);
    
    if (ret < 0) return (err == 0) ? -EIO : -err;
    
    fi->fh = (uint64_t)fh;
    return 0;
}

static int nfs_read_fuse(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    struct nfsfh *fh = (struct nfsfh *)fi->fh;
    pthread_mutex_lock(&main_ctx_lock);
    int ret = nfs_pread(main_ctx, fh, offset, size, buf);
    pthread_mutex_unlock(&main_ctx_lock);
    return ret;
}

static int nfs_readdir_fuse(const char *path, void *buf, fuse_fill_dir_t filler,
                            off_t offset, struct fuse_file_info *fi,
                            enum fuse_readdir_flags flags) {
    struct nfsdir *nfs_dir;
    struct nfsdirent *nfs_ent;
    pthread_mutex_lock(&main_ctx_lock);
    if (nfs_opendir(main_ctx, path, &nfs_dir) < 0) {
        int err = errno;
        pthread_mutex_unlock(&main_ctx_lock);
        return (err == 0) ? -EIO : -err;
    }
    while((nfs_ent = nfs_readdir(main_ctx, nfs_dir)) != NULL) {
        if(filler(buf, nfs_ent->name, NULL, 0, 0)) break;
    }
    nfs_closedir(main_ctx, nfs_dir);
    pthread_mutex_unlock(&main_ctx_lock);
    return 0;
}

static int nfs_chmod_fuse(const char *path, mode_t mode, struct fuse_file_info *fi) {
    pthread_mutex_lock(&main_ctx_lock);
    int ret = nfs_chmod(main_ctx, path, mode);
    int err = errno;
    pthread_mutex_unlock(&main_ctx_lock);
    return (ret < 0) ? -err : 0;
}

static int nfs_chown_fuse(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
    pthread_mutex_lock(&main_ctx_lock);
    int ret = nfs_chown(main_ctx, path, uid, gid);
    int err = errno;
    pthread_mutex_unlock(&main_ctx_lock);
    return (ret < 0) ? -err : 0;
}

static int nfs_truncate_fuse(const char *path, off_t size, struct fuse_file_info *fi) {
    pthread_mutex_lock(&main_ctx_lock);
    int ret = nfs_truncate(main_ctx, path, size);
    int err = errno;
    pthread_mutex_unlock(&main_ctx_lock);
    return (ret < 0) ? -err : 0;
}

static int nfs_utimens_fuse(const char *path, const struct timespec tv[2], struct fuse_file_info *fi) {
    struct timeval times[2];
    times[0].tv_sec = tv[0].tv_sec; times[0].tv_usec = tv[0].tv_nsec / 1000;
    times[1].tv_sec = tv[1].tv_sec; times[1].tv_usec = tv[1].tv_nsec / 1000;
    
    pthread_mutex_lock(&main_ctx_lock);
    int ret = nfs_utimes(main_ctx, path, times);
    int err = errno;
    pthread_mutex_unlock(&main_ctx_lock);
    return (ret < 0) ? -err : 0;
}

static void drain_writes() {
    while (atomic_load(&pending_write_count) > 0) usleep(1000); 
}

static int nfs_release_fuse(const char *path, struct fuse_file_info *fi) {
    struct nfsfh *fh = (struct nfsfh *)fi->fh;
    drain_writes(); 
    pthread_mutex_lock(&main_ctx_lock);
    if (fh) nfs_close(main_ctx, fh);
    pthread_mutex_unlock(&main_ctx_lock);
    return 0;
}

static struct fuse_operations nfs_oper = {
    .init       = nfs_init,
    .getattr    = nfs_getattr_fuse,
    .access     = nfs_access_fuse,
    .open       = nfs_open_fuse,
    .read       = nfs_read_fuse,
    .write      = nfs_write_fuse,
    .create     = nfs_create_fuse,
    .mkdir      = nfs_mkdir_fuse,
    .unlink     = nfs_unlink_fuse,
    .rmdir      = nfs_rmdir_fuse,
    .readdir    = nfs_readdir_fuse,
    .chmod      = nfs_chmod_fuse,
    .chown      = nfs_chown_fuse,
    .truncate   = nfs_truncate_fuse,
    .utimens    = nfs_utimens_fuse,
    .rename     = nfs_rename_fuse,
    .release    = nfs_release_fuse,
};

// --- Main ---

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);
    
    int new_argc = 0;
    char **new_argv = malloc(sizeof(char*) * argc);
    char *nfs_url_env = getenv("NFS_URL");

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--proto") == 0 && i + 1 < argc) {
            if (strcasecmp(argv[i+1], "rdma") == 0) {
                current_proto = PROTO_RDMA;
            } else {
                current_proto = PROTO_TCP;
            }
            i++; 
        } else {
            new_argv[new_argc++] = argv[i];
        }
    }

    if (!nfs_url_env) {
        fprintf(stderr, "Error: Set NFS_URL environment variable.\n");
        return 1;
    }

    if (current_proto == PROTO_RDMA) {
        printf("--- RDMA Mode Selected ---\n");
        char *preload = getenv("LD_PRELOAD");
        if (!preload || strstr(preload, "librspreload") == NULL) {
            fprintf(stderr, "WARNING: 'librspreload.so' not detected in LD_PRELOAD.\n");
        }
    }

    main_ctx = nfs_init_context();
    char robust_url[4096];
    char sep = strchr(nfs_url_env, '?') ? '&' : '?';

    if (current_proto == PROTO_RDMA) {
        snprintf(robust_url, sizeof(robust_url), 
             "%s%ctcp-keepalive=1&rsize=1048576&wsize=1048576", nfs_url_env, sep);
    } else {
        snprintf(robust_url, sizeof(robust_url), 
             "%s%ctcp-keepalive=1&rsize=131072&wsize=131072", nfs_url_env, sep);
    }

    struct nfs_url *urls = nfs_parse_url_dir(main_ctx, robust_url);
    if (!urls) { fprintf(stderr, "Invalid URL\n"); return 1; }

    global_server = strdup(urls->server);
    global_path = strdup(urls->path);

    if (nfs_mount(main_ctx, global_server, global_path) != 0) {
        fprintf(stderr, "Mount failed: %s\n", nfs_get_error(main_ctx));
        return 1;
    }
    
    return fuse_main(new_argc, new_argv, &nfs_oper, NULL);
}
