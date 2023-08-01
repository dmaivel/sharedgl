#include <linux/module.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/uaccess.h>

#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/mm.h>

struct bar_t {
	resource_size_t len;
	void __iomem *addr;
	resource_size_t phys;
};

struct pci_char {
	struct bar_t bar[6];
	dev_t major;
	struct cdev cdev;
};

static struct class *pchar_class;

static int dev_open(struct inode *inode, struct file *file)
{
	unsigned int num = iminor(file->f_path.dentry->d_inode);
	struct pci_char *pchar = container_of(inode->i_cdev, struct pci_char, cdev);

	if (num > 5)
		return -ENXIO;
	if (pchar->bar[num].len == 0)
		return -EIO;

	file->private_data = pchar;
	return 0;
};

int dev_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct pci_char *pchar = file->private_data;

	if (io_remap_pfn_range(vma, vma->vm_start,
            pchar->bar[2].phys >> PAGE_SHIFT, vma->vm_end - vma->vm_start,
            vma->vm_page_prot))
		return -EAGAIN;
	
	return 0;
}

static const struct file_operations fops = {
	.owner	 = THIS_MODULE,
	.open	 = dev_open,
	.mmap    = dev_mmap
};

static int my_dev_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	add_uevent_var(env, "DEVMODE=%#o", 0666);
	return 0;
}

static int pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	int err = 0, i;
	int mem_bars;
	struct pci_char *pchar;
	struct device *dev;
	dev_t dev_num;

	pchar = kmalloc(sizeof(struct pci_char), GFP_KERNEL);
	if (!pchar) {
		err = -ENOMEM;
		goto failure_kmalloc;
	}

	err = pci_enable_device_mem(pdev);
	if (err)
		goto failure_pci_enable;

	mem_bars = pci_select_bars(pdev, IORESOURCE_MEM);
	err = pci_request_selected_regions(pdev, mem_bars, "sharedgl");
	if (err)
		goto failure_pci_regions;

	for (i = 0; i < 6; i++) {
		if (mem_bars & (1 << i)) {
			pchar->bar[i].addr = ioremap(pci_resource_start(pdev, i), pci_resource_len(pdev, i));
			if (IS_ERR(pchar->bar[i].addr)) {
				err = PTR_ERR(pchar->bar[i].addr);
				break;
			} else {
				pchar->bar[i].phys = pci_resource_start(pdev, i);
				pchar->bar[i].len = pci_resource_len(pdev, i);
			}
		} else {
			pchar->bar[i].addr = NULL;
			pchar->bar[i].len = 0;
		}
	}

	if (err) {
		for (i--; i >= 0; i--)
			if (pchar->bar[i].len)
				iounmap(pchar->bar[i].addr);
		goto failure_ioremap;
	}

	err = alloc_chrdev_region(&dev_num, 0, 6, "sharedgl");
	if (err)
		goto failure_alloc_chrdev_region;

	pchar->major = MAJOR(dev_num);

	cdev_init(&pchar->cdev, &fops);
	pchar->cdev.owner = THIS_MODULE;

	err = cdev_add(&pchar->cdev, MKDEV(pchar->major, 0), 6);
	if (err)
		goto failure_cdev_add;

	pchar_class->dev_uevent = my_dev_uevent;
	for (i = 0; i < 6; i++) {
		if (pchar->bar[i].len) {
			dev = device_create(pchar_class, &pdev->dev,
					    MKDEV(pchar->major, i),
					    NULL, "b%xd%xf%x_bar%d",
					    pdev->bus->number,
					    PCI_SLOT(pdev->devfn),
					    PCI_FUNC(pdev->devfn), i);
			if (IS_ERR(dev)) {
				err = PTR_ERR(dev);
				break;
			}
		}
	}

	if (err) {
		for (i--; i >= 0; i--)
			if (pchar->bar[i].len)
				device_destroy(pchar_class,
					       MKDEV(pchar->major, i));
		goto failure_device_create;
	}

	pci_set_drvdata(pdev, pchar);
	dev_info(&pdev->dev, "claimed by sharedgl\n");

	return 0;

failure_device_create:
	cdev_del(&pchar->cdev);

failure_cdev_add:
	unregister_chrdev_region(MKDEV(pchar->major, 0), 6);

failure_alloc_chrdev_region:
	for (i = 0; i < 6; i++)
		if (pchar->bar[i].len)
			iounmap(pchar->bar[i].addr);

failure_ioremap:
	pci_release_selected_regions(pdev,
	pci_select_bars(pdev, IORESOURCE_MEM));

failure_pci_regions:
	pci_disable_device(pdev);

failure_pci_enable:
	kfree(pchar);

failure_kmalloc:
	return err;
}

static void pci_remove(struct pci_dev *pdev)
{
	int i;
	struct pci_char *pchar = pci_get_drvdata(pdev);

	for (i = 0; i < 6; i++)
		if (pchar->bar[i].len)
			device_destroy(pchar_class,
				       MKDEV(pchar->major, i));

	cdev_del(&pchar->cdev);

	unregister_chrdev_region(MKDEV(pchar->major, 0), 6);

	for (i = 0; i < 6; i++)
		if (pchar->bar[i].len)
			iounmap(pchar->bar[i].addr);

	pci_release_selected_regions(pdev, pci_select_bars(pdev, IORESOURCE_MEM));
	pci_disable_device(pdev);
	kfree(pchar);
}

static struct pci_device_id pci_ids[] = {
	{
		.vendor = 0x1af4,
		.device = 0x1110,
		.subvendor = PCI_ANY_ID,
		.subdevice = PCI_ANY_ID,
	},
	{ 0, }
};

static struct pci_driver pchar_driver = {
	.name		= "sharedgl",
	.id_table	= pci_ids,
	.probe		= pci_probe,
	.remove     = pci_remove,
};

static char *pci_char_devnode(const struct device *dev, umode_t *mode)
{
	struct pci_dev *pdev = to_pci_dev(dev->parent);
	return kasprintf(
        GFP_KERNEL, "sharedgl/%02x:%02x.%02x/bar%d",
        pdev->bus->number,
        PCI_SLOT(pdev->devfn),
        PCI_FUNC(pdev->devfn),
        MINOR(dev->devt)
    );
}

static int __init pci_init(void)
{
	int err;

	pchar_class = class_create(THIS_MODULE, "sharedgl"); // IF THIS LINE GIVES YOU AN ERROR, REMOVE 'THIS_MODULE'
	if (IS_ERR(pchar_class)) {
		err = PTR_ERR(pchar_class);
		return err;
	}
	
	pchar_class->devnode = pci_char_devnode;

	err = pci_register_driver(&pchar_driver);
	if (err)
		goto failure_register_driver;

	return 0;

failure_register_driver:
	class_destroy(pchar_class);

	return err;	
}

static void __exit pci_exit(void)
{
	pci_unregister_driver(&pchar_driver);
	class_destroy(pchar_class);
}

module_init(pci_init);
module_exit(pci_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Driver to expose sharedgl memory");
MODULE_AUTHOR("dmaivel");