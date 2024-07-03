#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/vmalloc.h>

#include <linux/of.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/irqdomain.h>

#include <linux/mem_encrypt.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_device.h>
#include <linux/set_memory.h>
#include <asm/rsi.h>
#include "portal.h"


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jaehyuk");
MODULE_DESCRIPTION("Platform device driver for portal");
MODULE_VERSION("0.1");

static irqreturn_t portal_dev_handler(int irq, void *dev_id)
{
	pr_info("Injected interrupt [%d] from KVM for portal", irq);

	//TODO{Device should be properly detached}

	//notify RMM the device has been detached from the realm
	//portal_detach_dev(0xdeadbeef);
	return IRQ_HANDLED;
}

static int portal_device_probe(struct platform_device *pdev) {
	
	// Check if a mapping already exists for the portal IRQ
	int irq_num; 
	int ret;

	pr_info("[%s] virtual device for portal is found\n",__func__);

	//the returned irq_num is kernel irq not hwirq
	if (!(irq_num = platform_get_irq_byname(pdev, "portal")))
		pr_info("parsing error, cannot find irq_num for portal\n");

	//subscribe irq 
	ret = request_irq(irq_num, portal_dev_handler,
			IRQF_ONESHOT, "portal device management", NULL);
	if (ret) {
		switch (ret) {
			case -EBUSY:
				printk(KERN_ERR "IRQ %d is busy\n", irq_num);
				break;
			case -EINVAL:
				printk(KERN_ERR "Invalid argument for %d\n", irq_num);
				break;
			case -ENOMEM:
				printk(KERN_ERR "Not enough memory for %d\n", irq_num);
				break;
			default:
				printk(KERN_ERR "Unknown error for %d\n", irq_num);
				break;

			return 0;
		}		
	} else {
		pr_info("IRQ %d is subscribed for portal!\n", irq_num);
	}

	//for testing
	portal_attach_dev(0xdeadbeef);
	return 0;

}

static const struct of_device_id portal_of_device_ids[] = {
	{.compatible = "arm,portal", .data = (void*) 1},
	{},
};

static void portal_driver_unregister(struct platform_driver *drv)
{
	platform_driver_unregister(drv);
	//TODO{detach all devices before unloading}
	printk(KERN_INFO "Goodbye, portal!\n");
}

static struct platform_driver portal_driver = {
	.driver = {
		.name = "portal",
		.of_match_table = portal_of_device_ids,
		.suppress_bind_attrs = true,
	},
	.probe = portal_device_probe,
};

module_driver(portal_driver, platform_driver_register,
	      portal_driver_unregister)

