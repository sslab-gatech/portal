#include <linux/init.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/vmalloc.h>

#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/irqdomain.h>

#include <linux/mem_encrypt.h>
#include <linux/set_memory.h>
#include <asm/rsi.h>


#define PORTAL_INTERRUPT_NUM 877

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("A simple example Linux kernel module.");
MODULE_VERSION("0.1");

static irqreturn_t portal_dev_handler(int irq, void *dev_id)
{
	pr_info("Injected interrupt [%d] from KVM for portal", irq);
	return IRQ_HANDLED;
}

void *aligned_kmalloc(size_t size, unsigned int alignment)
{
    void *ptr = kmalloc(size + alignment - 1, GFP_KERNEL);
    if (ptr) {
        uintptr_t addr = (uintptr_t)ptr + alignment - 1;
        ptr = (void *)(addr - (addr % alignment));
    }
    return ptr;
}

static int __init cca_test_init(void) {
	
	// Check if a mapping already exists for the portal IRQ
	unsigned int existing_irq = irq_find_mapping(NULL, PORTAL_INTERRUPT_NUM);

	if (existing_irq) {
		pr_err("A mapping already exists for virtual IRQ %u with usable IRQ %u\n", 
				PORTAL_INTERRUPT_NUM, existing_irq);
		return -ENXIO;
	}
	
	//create IRQ mapping 
	unsigned int irq_num = irq_create_mapping(NULL, PORTAL_INTERRUPT_NUM);
	if (!irq_num) {
		pr_err("Failed to create IRQ mapping for portal IRQ %u\n", PORTAL_INTERRUPT_NUM);
		return -ENXIO; 
	}

	//subscribe irq 
	int ret = request_irq(PORTAL_INTERRUPT_NUM, portal_dev_handler,
			IRQF_SHARED, "portal device management", NULL);
	if (ret) {
		switch (ret) {
			case -EBUSY:
				printk(KERN_ERR "IRQ %d is busy\n", PORTAL_INTERRUPT_NUM);
				break;
			case -EINVAL:
				printk(KERN_ERR "Invalid argument for %d\n", PORTAL_INTERRUPT_NUM);
				break;
			case -ENOMEM:
				printk(KERN_ERR "Not enough memory for %d\n", PORTAL_INTERRUPT_NUM);
				break;
			default:
				printk(KERN_ERR "Unknown error for %d\n", PORTAL_INTERRUPT_NUM);
				break;

			return 0;
		}		
	} else {
		pr_info("IRQ %d is subscribed for %d!!\n", irq_num, PORTAL_INTERRUPT_NUM);
	}

	//asking host to inject the fault
	portal_attach_dev(0xdeadbeef);

	pr_info("Interrupt handler registered successfully\n");
	return 0; // Non-zero return means that the module couldn't be loaded.
}

static void __exit cca_test_exit(void) {
    printk(KERN_INFO "Goodbye, world!\n");
}

module_init(cca_test_init);
module_exit(cca_test_exit);
