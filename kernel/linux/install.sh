mkdir /lib/modules/$(uname -r)/kernel/drivers/pci/sharedgl
zstd sharedgl.ko
mv sharedgl.ko.zst /lib/modules/$(uname -r)/kernel/drivers/pci/sharedgl/sharedgl.ko.zst
depmod -a
modprobe sharedgl