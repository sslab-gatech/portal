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

void __attribute__((aligned(4096))) test_func(void)
{
    printk(KERN_INFO "Executing my_function\n\n");
}

void __attribute__((aligned(4096))) test_func2(void)
{
    printk(KERN_INFO "Executing my_function\n\n");
}

static int __init hello_init(void) {
    void *codePage = kmalloc(4096, GFP_KERNEL);
    char nop_instruction[4] = {0x1f, 0x20, 0x03, 0xd5}; // ARM64 NOP instruction
    size_t num_nops = 4096 / sizeof(nop_instruction);
    void (*nop_function)(void) = (void (*)(void))codePage;
    size_t i = 0;

    printk(KERN_INFO "Hello, world!\n\n\n\n\n\n\n");
    printk("function 1: %p \t function2: %p\n", test_func, test_func2);
    
    // Calculate the number of NOP instructions needed to fill the page

    // Use memcpy to write NOP instructions to the page
    for (i = 0; i < num_nops; ++i) {
        memcpy((char *)codePage+ i * sizeof(nop_instruction), nop_instruction, sizeof(nop_instruction));
    }
    
    set_memory_x((unsigned long)codePage, 1);

    nop_function();





    //set_memory_portal_executable(test_func, 1);

    return 0; // Non-zero return means that the module couldn't be loaded.
}

static void __exit hello_exit(void) {
    printk(KERN_INFO "Goodbye, world!\n");
}

module_init(hello_init);
module_exit(hello_exit);
