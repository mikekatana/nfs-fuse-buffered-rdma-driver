# NFS Buffered IO FUSE Driver supporting RDMA

## Install IB Dependencies

sudo apt-get install rdma-core libibverbs1 ibverbs-providers librdmacm1

Ensure link is up using ibstat

##ibstat

```
`CA 'mlx5_0'
        CA type: MT4125
        Number of ports: 1
        Firmware version: 22.43.3608
        Hardware version: 0
        Node GUID: 0x58a2e10300e07c20
        System image GUID: 0x58a2e10300e07c20
        Port 1:
                State: Active
                Physical state: LinkUp
                Rate: 100
                Base lid: 0
                LMC: 0
                SM lid: 0
                Capability mask: 0x00010000
                Port GUID: 0x5aa2e1fffee07c20
                Link layer: Ethernet
```

## Check that all relevant modules are being loaded

#lsmod
Module                  Size  Used by
nfsv3                  61440  1
nfs_acl                12288  1 nfsv3
nfs                   569344  2 nfsv3
lockd                 143360  2 nfsv3,nfs
grace                  12288  1 lockd
netfs                 512000  1 nfs
rdma_ucm               32768  0
ib_ipoib              155648  0
ib_umad                45056  0
qrtr                   53248  2
cfg80211             1355776  0
binfmt_misc            24576  1
nls_iso8859_1          12288  1
intel_rapl_msr         20480  0
intel_rapl_common      40960  1 intel_rapl_msr
intel_uncore_frequency    16384  0
intel_uncore_frequency_common    16384  1 intel_uncore_frequency
i10nm_edac             24576  0
skx_edac_common        24576  1 i10nm_edac
nfit                   81920  1 skx_edac_common
x86_pkg_temp_thermal    20480  0
intel_powerclamp       24576  0
coretemp               24576  0
kvm_intel             487424  0
cmdlinepart            12288  0
spi_nor               163840  0
mtd                    98304  3 spi_nor,cmdlinepart
kvm                  1409024  1 kvm_intel
dax_hmem               16384  0
irqbypass              12288  1 kvm
cxl_acpi               24576  0
rapl                   20480  0
cxl_port               16384  0
ipmi_ssif              45056  0
acpi_power_meter       20480  0
ast                   118784  0
intel_cstate           24576  0
cxl_core              299008  2 cxl_port,cxl_acpi
isst_if_mmio           12288  0
isst_if_mbox_pci       12288  0
i2c_algo_bit           16384  1 ast
isst_if_common         24576  2 isst_if_mmio,isst_if_mbox_pci
i2c_i801               36864  0
spi_intel_pci          12288  0
intel_th_gth           24576  0
i2c_smbus              16384  1 i2c_i801
spi_intel              32768  1 spi_intel_pci
mei_me                 53248  0
intel_th_pci           12288  0
mei                   172032  1 mei_me
ioatdma                90112  0
intel_th               32768  2 intel_th_gth,intel_th_pci
intel_pch_thermal      20480  0
intel_vsec             20480  0
dca                    16384  1 ioatdma
ipmi_si                86016  1
acpi_ipmi              24576  1 acpi_power_meter
ipmi_devintf           16384  0
ipmi_msghandler        94208  4 ipmi_devintf,ipmi_si,acpi_ipmi,ipmi_ssif
acpi_pad              184320  0
joydev                 32768  0
input_leds             12288  0
mac_hid                12288  0
sch_fq_codel           24576  160
rpcrdma                98304  2
rdma_cm               147456  2 rpcrdma,rdma_ucm
iw_cm                  61440  1 rdma_cm
ib_cm                 147456  2 rdma_cm,ib_ipoib
sunrpc                802816  17 rpcrdma,lockd,nfsv3,nfs_acl,nfs
dm_multipath           45056  0
knem                   49152  0
msr                    12288  0
efi_pstore             12288  0
nfnetlink              20480  2
dmi_sysfs              24576  0
ip_tables              32768  0
x_tables               65536  1 ip_tables
autofs4                57344  2
btrfs                2043904  0
blake2b_generic        24576  0
raid10                 73728  0
raid456               192512  0
async_raid6_recov      20480  1 raid456
async_memcpy           16384  2 raid456,async_raid6_recov
async_pq               20480  2 raid456,async_raid6_recov
async_xor              16384  3 async_pq,raid456,async_raid6_recov
async_tx               16384  5 async_pq,async_memcpy,async_xor,raid456,async_raid6_recov
xor                    20480  2 async_xor,btrfs
raid6_pq              126976  4 async_pq,btrfs,raid456,async_raid6_recov
libcrc32c              12288  2 btrfs,raid456
raid1                  57344  0
raid0                  24576  0
mlx5_ib               548864  0
ib_uverbs             200704  2 rdma_ucm,mlx5_ib
macsec                 77824  1 mlx5_ib
ib_core               524288  9 rdma_cm,ib_ipoib,rpcrdma,iw_cm,ib_umad,rdma_ucm,ib_uverbs,mlx5_ib,ib_cm
hid_generic            12288  0
usbhid                 77824  0
hid                   180224  2 usbhid,hid_generic

# Install FUSE dependencies
sudo apt install build-essential cmake libfuse3-dev libnfs-dev pkg-config



mlx5_core            2809856  1 mlx5_ib
mlxfw                  36864  1 mlx5_core
