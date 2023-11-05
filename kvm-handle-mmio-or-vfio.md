```cpp
**Input**
fault_ipa: fault ipa of the guest (page granule)
memslot: memslot translating fault_ipa to hva 
hva: hva mapped to fault_ipa (already translated by the caller through memslot)


 static int user_mem_abort(struct kvm_vcpu *vcpu, phys_addr_t fault_ipa,
                          struct kvm_memory_slot *memslot, unsigned long hva,
                          unsigned long fault_status)
{       
        int ret = 0;
        bool write_fault, writable, force_pte = false;
        bool exec_fault;
        bool device = false;
        unsigned long mmu_seq;
        struct kvm *kvm = vcpu->kvm;
        struct kvm_mmu_memory_cache *memcache = &vcpu->arch.mmu_page_cache;
        struct vm_area_struct *vma;
        short vma_shift;
        gfn_t gfn;
        kvm_pfn_t pfn;
        bool logging_active = memslot_is_logging(memslot);
        unsigned long fault_level = kvm_vcpu_trap_get_fault_level(vcpu);
        unsigned long vma_pagesize, fault_granule;
        enum kvm_pgtable_prot prot = KVM_PGTABLE_PROT_R;
        struct kvm_pgtable *pgt;
        gpa_t gpa_stolen_mask = kvm_gpa_stolen_bits(vcpu->kvm);
        
        fault_granule = 1UL << ARM64_HW_PGTABLE_LEVEL_SHIFT(fault_level);
        write_fault = kvm_is_write_fault(vcpu);
        
        /* Realms cannot map read-only */
        if (vcpu_is_rec(vcpu))
                write_fault = true;
        
        exec_fault = kvm_vcpu_trap_is_exec_fault(vcpu);
        VM_BUG_ON(write_fault && exec_fault);
        
        if (fault_status == FSC_PERM && !write_fault && !exec_fault) {
                kvm_err("Unexpected L2 read permission error\n");
                return -EFAULT;
        }
        
        /*
         * Let's check if we will get back a huge page backed by hugetlbfs, or
         * get block mapping for device MMIO region.
         */
        mmap_read_lock(current->mm);
        vma = vma_lookup(current->mm, hva);
        if (unlikely(!vma)) {
                kvm_err("Failed to find VMA for hva 0x%lx\n", hva);
                mmap_read_unlock(current->mm);
                return -EFAULT;
        }
        
        /*
         * logging_active is guaranteed to never be true for VM_PFNMAP
         * memslots.
         */
        if (logging_active) {
                force_pte = true;
                vma_shift = PAGE_SHIFT;
        } else if (kvm_is_realm(kvm)) {
                // Force PTE level mappings for realms
                force_pte = true;
                vma_shift = PAGE_SHIFT; 
        } else {
                vma_shift = get_vma_page_shift(vma, hva);
        }
        
        switch (vma_shift) {
#ifndef __PAGETABLE_PMD_FOLDED
        case PUD_SHIFT: 
                if (fault_supports_stage2_huge_mapping(memslot, hva, PUD_SIZE))
                        break;
                fallthrough;
#endif  
        case CONT_PMD_SHIFT:
                vma_shift = PMD_SHIFT;
                fallthrough;
        case PMD_SHIFT:
                if (fault_supports_stage2_huge_mapping(memslot, hva, PMD_SIZE))
                        break;
                fallthrough;
        case CONT_PTE_SHIFT:  
                vma_shift = PAGE_SHIFT;
                force_pte = true;
                fallthrough;
        case PAGE_SHIFT:
                break;
        default:
                WARN_ONCE(1, "Unknown vma_shift %d", vma_shift);
        }

        vma_pagesize = 1UL << vma_shift;
        if (vma_pagesize == PMD_SIZE || vma_pagesize == PUD_SIZE)
                fault_ipa &= ~(vma_pagesize - 1);

        gfn = (fault_ipa & ~gpa_stolen_mask) >> PAGE_SHIFT;
        mmap_read_unlock(current->mm);

        /*
         * Permission faults just need to update the existing leaf entry,
         * and so normally don't require allocations from the memcache. The
         * only exception to this is when dirty logging is enabled at runtime
         * and a write fault needs to collapse a block entry into a table.
         */
        if (fault_status != FSC_PERM || (logging_active && write_fault)) {
                ret = kvm_mmu_topup_memory_cache(memcache,
                                                 kvm_mmu_cache_min_pages(kvm));
                if (ret)
                        return ret;
        }

        mmu_seq = vcpu->kvm->mmu_invalidate_seq;
        /*
         * Ensure the read of mmu_invalidate_seq happens before we call
         * gfn_to_pfn_prot (which calls get_user_pages), so that we don't risk
         * the page we just got a reference to gets unmapped before we have a
         * chance to grab the mmu_lock, which ensure that if the page gets
         * unmapped afterwards, the call to kvm_unmap_gfn will take it away
         * from us again properly. This smp_rmb() interacts with the smp_wmb()
         * in kvm_mmu_notifier_invalidate_<page|range_end>.
         *
         * Besides, __gfn_to_pfn_memslot() instead of gfn_to_pfn_prot() is
         * used to avoid unnecessary overhead introduced to locate the memory
         * slot because it's always fixed even @gfn is adjusted for huge pages.
         */
        smp_rmb();
        
        pfn = __gfn_to_pfn_memslot(memslot, gfn, false, false, NULL,
                                   write_fault, &writable, NULL);
        if (pfn == KVM_PFN_ERR_HWPOISON) {
                kvm_send_hwpoison_signal(hva, vma_shift);
                return 0;
        }
        if (is_error_noslot_pfn(pfn))
                return -EFAULT;
        
        if (kvm_is_device_pfn(pfn)) {
                /*
                 * If the page was identified as device early by looking at
                 * the VMA flags, vma_pagesize is already representing the
                 * largest quantity we can map.  If instead it was mapped
                 * via gfn_to_pfn_prot(), vma_pagesize is set to PAGE_SIZE
                 * and must not be upgraded.
                 *
                 * In both cases, we don't let transparent_hugepage_adjust()
                 * change things at the last minute.
                 */
                printk("device addr: %llx -> %llx\n", fault_ipa, 
                                (fault_ipa | (kvm_vcpu_get_hfar(vcpu) & ((1 << 12) - 1))) & ~gpa_stolen_mask);
                
                device = true;
        } else if (logging_active && !write_fault) {
                /*
                 * Only actually map the page as writable if this was a write
                 * fault.
                 */
                writable = false;
        }
        
        
        if (exec_fault && device)
                return -ENOEXEC;

        read_lock(&kvm->mmu_lock);
        pgt = vcpu->arch.hw_mmu->pgt;
        if (mmu_invalidate_retry(kvm, mmu_seq))
                goto out_unlock;

        /*
         * If we are not forced to use page mapping, check if we are
         * backed by a THP and thus use block mapping if possible.
         */
        /* FIXME: We shouldn't need to disable this for realms */
        if (vma_pagesize == PAGE_SIZE && !(force_pte || device || kvm_is_realm(kvm))) {
                if (fault_status == FSC_PERM && fault_granule > PAGE_SIZE)
                        vma_pagesize = fault_granule;
                else
                        vma_pagesize = transparent_hugepage_adjust(kvm, memslot,
                                                                   hva, &pfn,
                                                                   &fault_ipa);
        }

        if (fault_status != FSC_PERM && !device && kvm_has_mte(kvm)) {
                /* Check the VMM hasn't introduced a new disallowed VMA */
                if (kvm_vma_mte_allowed(vma)) {
                        sanitise_mte_tags(kvm, pfn, vma_pagesize);
                } else {
                        ret = -EFAULT;
                        goto out_unlock;
                }
        }

        if (writable)
                prot |= KVM_PGTABLE_PROT_W;

        if (exec_fault)
                prot |= KVM_PGTABLE_PROT_X;

        if (device)
                prot |= KVM_PGTABLE_PROT_DEVICE;
        else if (cpus_have_const_cap(ARM64_HAS_CACHE_DIC))
                prot |= KVM_PGTABLE_PROT_X;

        /*
         * Under the premise of getting a FSC_PERM fault, we just need to relax
         * permissions only if vma_pagesize equals fault_granule. Otherwise,
         * kvm_pgtable_stage2_map() should be called to change block size.
         */
        if (fault_status == FSC_PERM && vma_pagesize == fault_granule)
                ret = kvm_pgtable_stage2_relax_perms(pgt, fault_ipa, prot);
        else if (kvm_is_realm(kvm))
                ret = realm_map_ipa(kvm, fault_ipa, hva, pfn, vma_pagesize,
                                    prot, memcache);
        else
                ret = kvm_pgtable_stage2_map(pgt, fault_ipa, vma_pagesize,
                                             __pfn_to_phys(pfn), prot,
                                             memcache, KVM_PGTABLE_WALK_SHARED);

        /* Mark the page dirty only if the fault is handled successfully */
        if (writable && !ret) {
                kvm_set_pfn_dirty(pfn);
                mark_page_dirty_in_slot(kvm, memslot, gfn);
        }
                
out_unlock:
        read_unlock(&kvm->mmu_lock);
        kvm_set_pfn_accessed(pfn);
        kvm_release_pfn_clean(pfn);
        return ret != -EAGAIN ? ret : 0;
}       


```

### Per VM memory region structure for maintaining information

```cpp
struct vm_area_struct {
        /* The first cache line has the info for VMA tree walking. */
        
        unsigned long vm_start;         /* Our start address within vm_mm. */
        unsigned long vm_end;           /* The first byte after our end address
                                           within vm_mm. */

        struct mm_struct *vm_mm;        /* The address space we belong to. */

        /* 
         * Access permissions of this VMA.
         * See vmf_insert_mixed_prot() for discussion.
         */             
        pgprot_t vm_page_prot;
        unsigned long vm_flags;         /* Flags, see mm.h. */

        /*
         * For areas with an address space and backing store,
         * linkage into the address_space->i_mmap interval tree.
         *
         */
        struct {
                struct rb_node rb;
                unsigned long rb_subtree_last;
        } shared;       
        
        /*
         * A file's MAP_PRIVATE vma can be in both i_mmap tree and anon_vma
         * list, after a COW of one of the file pages.  A MAP_SHARED vma
         * can only be in the i_mmap tree.  An anonymous MAP_PRIVATE, stack
         * or brk vma (with NULL file) can only be in an anon_vma list.
         */
        struct list_head anon_vma_chain; /* Serialized by mmap_lock &
                                          * page_table_lock */
        struct anon_vma *anon_vma;      /* Serialized by page_table_lock */

        /* Function pointers to deal with this struct. */
        const struct vm_operations_struct *vm_ops;

        /* Information about our backing store: */
        unsigned long vm_pgoff;         /* Offset (within vm_file) in PAGE_SIZE
                                           units */
        struct file * vm_file;          /* File we map to (can be NULL). */
        void * vm_private_data;         /* was vm_pte (shared mem) */

#ifdef CONFIG_ANON_VMA_NAME
        /*
         * For private and shared anonymous mappings, a pointer to a null
         * terminated string containing the name given to the vma, or NULL if
         * unnamed. Serialized by mmap_sem. Use anon_vma_name to access.
         */
        struct anon_vma_name *anon_name;
#endif
#ifdef CONFIG_SWAP
        atomic_long_t swap_readahead_info;
#endif
#ifndef CONFIG_MMU
        struct vm_region *vm_region;    /* NOMMU mapping region */
#endif
#ifdef CONFIG_NUMA
        struct mempolicy *vm_policy;    /* NUMA policy for the VMA */
#endif
        struct vm_userfaultfd_ctx vm_userfaultfd_ctx;
} __randomize_layout; 


```


```cpp
static bool kvm_is_device_pfn(unsigned long pfn)
{               
        return !pfn_is_map_memory(pfn);
}

#define PFN_PHYS(x)     ((phys_addr_t)(x) << PAGE_SHIFT)
#define PHYS_PFN(x)     ((unsigned long)((x) >> PAGE_SHIFT))
int pfn_is_map_memory(unsigned long pfn)
{
        phys_addr_t addr = PFN_PHYS(pfn);

        /* avoid false positives for bogus PFNs, see comment in pfn_valid() */
        if (PHYS_PFN(addr) != pfn)
                return 0;

        return memblock_is_map_memory(addr);
}
```


### Walking stage 2 page table 

```cpp
/*
 * The TABLE_PRE callback runs for table entries on the way down, looking
 * for table entries which we could conceivably replace with a block entry
 * for this mapping. If it finds one it replaces the entry and calls
 * kvm_pgtable_mm_ops::free_removed_table() to tear down the detached table.
 *
 * Otherwise, the LEAF callback performs the mapping at the existing leaves
 * instead.
 */
static int stage2_map_walker(const struct kvm_pgtable_visit_ctx *ctx,
                             enum kvm_pgtable_walk_flags visit)
{
        struct stage2_map_data *data = ctx->arg;

        switch (visit) {
        case KVM_PGTABLE_WALK_TABLE_PRE:
                return stage2_map_walk_table_pre(ctx, data);
        case KVM_PGTABLE_WALK_LEAF:
                return stage2_map_walk_leaf(ctx, data);
        default:
                return -EINVAL;
        }
}



```
