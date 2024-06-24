#include <linux/init.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/vmalloc.h>

#include <linux/mem_encrypt.h>
#include <linux/set_memory.h>
#include <asm/rsi.h>


#define INTERRUPT_NUMBER 777

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
	int ret = request_irq(INTERRUPT_NUMBER, portal_dev_handler,
			IRQF_SHARED, "portal device management", NULL);
	
	if (ret) {
		pr_err("Failed to register IRQ handler for portal");
		return ret;
	}

	pr_info("Interrupt handler registered successfully\n");
	return 0; // Non-zero return means that the module couldn't be loaded.
}

static void __exit cca_test_exit(void) {
    printk(KERN_INFO "Goodbye, world!\n");
}

module_init(cca_test_init);
module_exit(cca_test_exit);
