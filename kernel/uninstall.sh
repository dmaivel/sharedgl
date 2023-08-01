rmmod sharedgl
rm -rf /lib/modules/$(uname -r)/kernel/drivers/pci/sharedgl
depmod -a