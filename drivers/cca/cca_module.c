#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/vmalloc.h>

#include <linux/mem_encrypt.h>
#include <linux/set_memory.h>
#include <asm/rsi.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("A simple example Linux kernel module.");
MODULE_VERSION("0.1");

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


    return 0; // Non-zero return means that the module couldn't be loaded.
}

static void __exit cca_test_exit(void) {
    printk(KERN_INFO "Goodbye, world!\n");
}

module_init(cca_test_init);
module_exit(cca_test_exit);
