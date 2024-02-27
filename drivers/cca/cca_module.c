#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/vmalloc.h>
#include <asm/rsi.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("A simple example Linux kernel module.");
MODULE_VERSION("0.1");


__attribute__((aligned(4096))) void test_func(void)
{
    printk(KERN_INFO "Executing my_function\n\n");
}

__attribute__((aligned(4096))) void test_func2(void) 
{
    printk(KERN_INFO "Executing my_function\n\n");
}

static int __init hello_init(void) {
    printk(KERN_INFO "Hello, world!\n\n\n\n\n\n\n");

    printk("function 1: %p \t function2: %p\n",
		    test_func, test_func2);
    set_memory_portal_executable(unsigned long test_func, 1);



    return 0; // Non-zero return means that the module couldn't be loaded.
}

static void __exit hello_exit(void) {
    printk(KERN_INFO "Goodbye, world!\n");
}

module_init(hello_init);
module_exit(hello_exit);
