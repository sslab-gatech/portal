The key is PROT_NS_SHARED

### IO
```cpp
void set_fixmap_io(enum fixed_addresses idx, phys_addr_t phys)
{
        pgprot_t prot = FIXMAP_PAGE_IO;

        /*
         * For now we consider all I/O as non-secure. For future
         * filter the I/O base for setting appropriate permissions.
         */
        prot = __pgprot(pgprot_val(prot) | PROT_NS_SHARED);

        return __set_fixmap(idx, phys, prot);
}
```

### ioremap_prot
```cpp
/*
 * I/O memory mapping functions.
 */

bool ioremap_allowed(phys_addr_t phys_addr, size_t size, unsigned long prot);
#define ioremap_allowed ioremap_allowed

#define _PAGE_IOREMAP (PROT_DEVICE_nGnRE | PROT_NS_SHARED)

#define ioremap_wc(addr, size)  \
        ioremap_prot((addr), (size), (PROT_NORMAL_NC | PROT_NS_SHARED))
#define ioremap_np(addr, size)  \
        ioremap_prot((addr), (size), (PROT_DEVICE_nGnRnE | PROT_NS_SHARED))
```


```cpp
static inline void __iomem *ioremap(phys_addr_t addr, size_t size)
{
        /* _PAGE_IOREMAP needs to be supplied by the architecture */
        return ioremap_prot(addr, size, _PAGE_IOREMAP);
}

void __iomem *ioremap_prot(phys_addr_t phys_addr, size_t size,
                           unsigned long prot)
{       
        unsigned long offset, vaddr;
        phys_addr_t last_addr;
        struct vm_struct *area;
        
        /* Disallow wrap-around or zero size */
        last_addr = phys_addr + size - 1;
        if (!size || last_addr < phys_addr)
                return NULL;
        
        /* Page-align mappings */
        offset = phys_addr & (~PAGE_MASK);
        phys_addr -= offset;
        size = PAGE_ALIGN(size + offset);
        
        if (!ioremap_allowed(phys_addr, size, prot))
                return NULL;
        
        area = get_vm_area_caller(size, VM_IOREMAP,
                        __builtin_return_address(0));
        if (!area)
                return NULL;
        vaddr = (unsigned long)area->addr;
        area->phys_addr = phys_addr;
        
        if (ioremap_page_range(vaddr, vaddr + size, phys_addr,
                               __pgprot(prot))) {
                free_vm_area(area);
                return NULL;
        }
        
        return (void __iomem *)(vaddr + offset);
}
```

```cpp
#define pgprot_device(prot) \
        __pgprot_modify(prot, PTE_ATTRINDX_MASK, PTE_ATTRINDX(MT_DEVICE_nGnRE) | PTE_PXN | PTE_UXN | PROT_NS_SHARED)
```


### EFI
```cpp
/*
 * Only regions of type EFI_RUNTIME_SERVICES_CODE need to be
 * executable, everything else can be mapped with the XN bits
 * set. Also take the new (optional) RO/XP bits into account.
 */
static __init pteval_t create_mapping_protection(efi_memory_desc_t *md)
{
        u64 attr = md->attribute;
        u32 type = md->type;

        if (type == EFI_MEMORY_MAPPED_IO)
                return PROT_NS_SHARED | PROT_DEVICE_nGnRE;

        if (region_is_misaligned(md)) {
                static bool __initdata code_is_misaligned;

                /*
                 * Regions that are not aligned to the OS page size cannot be
                 * mapped with strict permissions, as those might interfere
                 * with the permissions that are needed by the adjacent
                 * region's mapping. However, if we haven't encountered any
                 * misaligned runtime code regions so far, we can safely use
                 * non-executable permissions for non-code regions.
                 */
                code_is_misaligned |= (type == EFI_RUNTIME_SERVICES_CODE);

                return code_is_misaligned ? pgprot_val(PAGE_KERNEL_EXEC)
                                          : pgprot_val(PAGE_KERNEL);
        }
        
        /* R-- */ 
        if ((attr & (EFI_MEMORY_XP | EFI_MEMORY_RO)) ==
            (EFI_MEMORY_XP | EFI_MEMORY_RO))
                return pgprot_val(PAGE_KERNEL_RO);
        
        /* R-X */
        if (attr & EFI_MEMORY_RO)
                return pgprot_val(PAGE_KERNEL_ROX);
        
        /* RW- */
        if (((attr & (EFI_MEMORY_RP | EFI_MEMORY_WP | EFI_MEMORY_XP)) ==
             EFI_MEMORY_XP) ||
            type != EFI_RUNTIME_SERVICES_CODE)
                return pgprot_val(PAGE_KERNEL);
        
        /* RWX */
        return pgprot_val(PAGE_KERNEL_EXEC);
}




```
