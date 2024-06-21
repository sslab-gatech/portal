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

static int __init hello_init(void) {
    void *codePage = aligned_kmalloc(4096, 4096);
    char nop_instruction[4] = {0x1f, 0x20, 0x03, 0xd5}; // ARM64 NOP instruction
    char ret_instruction[4] = {0xc0, 0x03, 0x5f, 0xd6};

    void (*nop_function)(void) = (void (*)(void))codePage;
    size_t i = 0;

    printk(KERN_INFO "Hello, world!\n\n\n\n\n\n\n");
    printk(KERN_INFO "codePage address: %lx\n", (unsigned long)codePage);
    
    // Calculate the number of NOP instructions needed to fill the page

    memcpy((char *)codePage, nop_instruction, sizeof(nop_instruction));
    memcpy((char *)codePage+sizeof(nop_instruction), ret_instruction, sizeof(ret_instruction));
    
    printk("Reading exectuable function first 4 char: ");
    for (i = 0; i < 8; i++) {
	    printk("%x ", *((char *)(codePage) + i));
    }
    printk("\n");

    set_memory_portal_executable((unsigned long)codePage, 1);

    memcpy((char *)codePage, nop_instruction, sizeof(nop_instruction));
    memcpy((char *)codePage+sizeof(nop_instruction), ret_instruction, sizeof(ret_instruction));
    
    printk("Reading exectuable function first 4 char: ");
    for (i = 0; i < 8; i++) {
	    printk("%x ", *((char *)(codePage) + i));
    }
    printk("\n");

    printk("before execution!\n");
    nop_function();
    printk("execution passed!\n");


    return 0; // Non-zero return means that the module couldn't be loaded.
}

static void __exit hello_exit(void) {
    printk(KERN_INFO "Goodbye, world!\n");
}

module_init(hello_init);
module_exit(hello_exit);
