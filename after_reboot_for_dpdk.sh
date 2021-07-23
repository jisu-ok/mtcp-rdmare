# Enable no-IOMMU mode
echo 1 | sudo tee /sys/module/vfio/parameters/enable_unsafe_noiommu_mode

# sudo ifconfig dpdk0 10.1.0.1 netmask 255.255.255.0 up
# export RTE_SDK=/home/jisu/mtcp-rdma/dpdk
# export RTE_TARGET=x86_64-native-linuxapp-gcc