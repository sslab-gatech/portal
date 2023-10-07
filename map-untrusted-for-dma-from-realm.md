## Realm Side
### DMA related allocations
```cpp                                                                          
static inline void *dma_alloc_coherent(struct device *dev, size_t size,         
                dma_addr_t *dma_handle, gfp_t gfp)                              
{                                                                               
        return dma_alloc_attrs(dev, size, dma_handle, gfp,                      
                        (gfp & __GFP_NOWARN) ? DMA_ATTR_NO_WARN : 0);           
}           
```

```cpp
void *dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *dma_handle,  
                gfp_t flag, unsigned long attrs)                                
{                                                                               
        const struct dma_map_ops *ops = get_dma_ops(dev);                       
        void *cpu_addr;                                                         
                                                                                
        WARN_ON_ONCE(!dev->coherent_dma_mask);                                  
                                                                                
        /*                                                                      
         * DMA allocations can never be turned back into a page pointer, so     
         * requesting compound pages doesn't make sense (and can't even be      
         * supported at all by various backends).                               
         */                                                                     
        if (WARN_ON_ONCE(flag & __GFP_COMP))                                    
                return NULL;                                                    
                                                                                
        if (dma_alloc_from_dev_coherent(dev, size, dma_handle, &cpu_addr))      
                return cpu_addr;                                                
                                                                                
        /* let the implementation decide on the zone to allocate from: */       
        flag &= ~(__GFP_DMA | __GFP_DMA32 | __GFP_HIGHMEM);                     
                                                                                
        if (dma_alloc_direct(dev, ops))                                         
                cpu_addr = dma_direct_alloc(dev, size, dma_handle, flag, attrs);
        else if (ops->alloc)                                                    
                cpu_addr = ops->alloc(dev, size, dma_handle, flag, attrs);      
        else                                                                    
                return NULL;                                                    
                                                                                
        debug_dma_alloc_coherent(dev, size, *dma_handle, cpu_addr, attrs);      
        return cpu_addr;                                                        
}
```

```cpp
void *dma_direct_alloc(struct device *dev, size_t size,
                dma_addr_t *dma_handle, gfp_t gfp, unsigned long attrs)
{
        bool remap = false, set_uncached = false;
        struct page *page;
        void *ret;      
        
        size = PAGE_ALIGN(size);
        if (attrs & DMA_ATTR_NO_WARN)
                gfp |= __GFP_NOWARN;
        
        if ((attrs & DMA_ATTR_NO_KERNEL_MAPPING) &&
            !force_dma_unencrypted(dev) && !is_swiotlb_for_alloc(dev))
                return dma_direct_alloc_no_mapping(dev, size, dma_handle, gfp);
                
        if (!dev_is_dma_coherent(dev)) {
                /*
                 * Fallback to the arch handler if it exists.  This should
                 * eventually go away.
                 */
                if (!IS_ENABLED(CONFIG_ARCH_HAS_DMA_SET_UNCACHED) &&
                    !IS_ENABLED(CONFIG_DMA_DIRECT_REMAP) &&
                    !IS_ENABLED(CONFIG_DMA_GLOBAL_POOL) &&
                    !is_swiotlb_for_alloc(dev)) 
                        return arch_dma_alloc(dev, size, dma_handle, gfp,
                                              attrs);
                
                /*
                 * If there is a global pool, always allocate from it for
                 * non-coherent devices.
                 */
                if (IS_ENABLED(CONFIG_DMA_GLOBAL_POOL))
                        return dma_alloc_from_global_coherent(dev, size,
                                        dma_handle);
        
                /*
                 * Otherwise remap if the architecture is asking for it.  But
                 * given that remapping memory is a blocking operation we'll
                 * instead have to dip into the atomic pools.
                 */
                remap = IS_ENABLED(CONFIG_DMA_DIRECT_REMAP);
                if (remap) {
                        if (dma_direct_use_pool(dev, gfp))
                                return dma_direct_alloc_from_pool(dev, size,
                                                dma_handle, gfp);
                } else {
                        if (!IS_ENABLED(CONFIG_ARCH_HAS_DMA_SET_UNCACHED))
                                return NULL;
                        set_uncached = true;
                }
        }

        /* 
         * Decrypting memory may block, so allocate the memory from the atomic
         * pools if we can't block.
         */     
        if (force_dma_unencrypted(dev) && dma_direct_use_pool(dev, gfp)) {
                return dma_direct_alloc_from_pool(dev, size, dma_handle, gfp);
        }

	/* we always manually zero the memory once we are done */
        page = __dma_direct_alloc_pages(dev, size, gfp & ~__GFP_ZERO, true);
        if (!page)
                return NULL;

        /*
         * dma_alloc_contiguous can return highmem pages depending on a
         * combination the cma= arguments and per-arch setup.  These need to be
         * remapped to return a kernel virtual address.
         */
        if (PageHighMem(page)) {
                remap = true;
                set_uncached = false;
        }

        if (remap) {
                pgprot_t prot = dma_pgprot(dev, PAGE_KERNEL, attrs);

                if (force_dma_unencrypted(dev))
                        prot = pgprot_decrypted(prot);

                /* remove any dirty cache lines on the kernel alias */
                arch_dma_prep_coherent(page, size);

                /* create a coherent mapping */
                ret = dma_common_contiguous_remap(page, size, prot,
                                __builtin_return_address(0));
                if (!ret)
                        goto out_free_pages;
        } else {
                ret = page_address(page);
                if (dma_set_decrypted(dev, ret, size))
                        goto out_free_pages;
        }

        memset(ret, 0, size);

        if (set_uncached) {
                arch_dma_prep_coherent(page, size);
                ret = arch_dma_set_uncached(ret, size);
                if (IS_ERR(ret))
                        goto out_encrypt_pages;
        }

        *dma_handle = phys_to_dma_direct(dev, page_to_phys(page));
        return ret;

out_encrypt_pages:
        if (dma_set_encrypted(dev, page_address(page), size))
                return NULL;
out_free_pages:
        __dma_direct_free_pages(dev, page, size);
        return NULL;
}
```




```cpp
static int dma_set_decrypted(struct device *dev, void *vaddr, size_t size)
{       
        if (!force_dma_unencrypted(dev))
                return 0;
        return set_memory_decrypted((unsigned long)vaddr, PFN_UP(size));
}       

static int dma_set_encrypted(struct device *dev, void *vaddr, size_t size)
{
        int ret;
                
        if (!force_dma_unencrypted(dev))
                return 0;
        ret = set_memory_encrypted((unsigned long)vaddr, PFN_UP(size));
        if (ret)
                pr_warn_ratelimited("leaking DMA memory that can't be re-encrypted\n");
        return ret; 
}      

int set_memory_encrypted(unsigned long addr, int numpages)
{
        return __set_memory_encrypted(addr, numpages, true);
}

int set_memory_decrypted(unsigned long addr, int numpages)
{
        return __set_memory_encrypted(addr, numpages, false);
}
```

```cpp
static int __set_memory_encrypted(unsigned long addr,
                                  int numpages,
                                  bool encrypt)
{
        unsigned long set_prot = 0, clear_prot = 0;
        phys_addr_t start, end;

        if (!is_realm_world())
                return 0;

        WARN_ON(!__is_lm_address(addr));
        start = __virt_to_phys(addr);
        end = start + numpages * PAGE_SIZE;

        if (encrypt) {
                clear_prot = PROT_NS_SHARED;
                set_memory_range_protected(start, end);
        } else {
                set_prot = PROT_NS_SHARED;
                set_memory_range_shared(start, end);
        }

        return __change_memory_common(addr, PAGE_SIZE * numpages,
                                      __pgprot(set_prot),
                                      __pgprot(clear_prot));
}
```


### Changing RIPAS
As the RIPAS is the indicator of where the memory belongs to in terms of IPA, 
it should first change the RIPAS of the mapped address properly. 

```cpp
static inline void set_memory_range_protected(phys_addr_t start, phys_addr_t end)
{       
        set_memory_range(start, end, RSI_RIPAS_RAM);
}

static inline void set_memory_range_shared(phys_addr_t start, phys_addr_t end)
{
        set_memory_range(start, end, RSI_RIPAS_EMPTY);
}
```

```cpp
static inline void set_memory_range(phys_addr_t start, phys_addr_t end,
                                    enum ripas state)
{
        unsigned long ret;
        phys_addr_t top;

        while (start != end) {
                ret = rsi_set_addr_range_state(start, end, state, &top);
                BUG_ON(ret);
                BUG_ON(top < start);
                BUG_ON(top > end);
                start = top;                         
        }                                            
}       
```

Because RIPAS can only be changed by the RMM, the REALM should ask RMM to change 
the RIPAS on behalf of it. To this end, it invokes the SMC call to ask RMM to 
handle RSI call. 

```cpp
static inline unsigned long rsi_set_addr_range_state(phys_addr_t start,
                                                     phys_addr_t end,
                                                     enum ripas state,
                                                     phys_addr_t *top)
{
        struct arm_smccc_res res;

        invoke_rsi_fn_smc_with_res(SMC_RSI_IPA_STATE_SET,
                                   start, (end - start), state, 0, &res);

        *top = res.a1;
        return res.a0;
}       

```

```cpp
static inline void invoke_rsi_fn_smc_with_res(unsigned long function_id,
                                              unsigned long arg0,
                                              unsigned long arg1,
                                              unsigned long arg2,
                                              unsigned long arg3,
                                              struct arm_smccc_res *res)
{
        arm_smccc_smc(function_id, arg0, arg1, arg2, arg3, 0, 0, 0, res);
}

```


## RMM Side
In high-level, when the Realm exits it returns to the RMM and returns to the
rec_run_loop function and invokes handle_realm_exit function to handle the fault
inside the RMM.

```cpp
void rec_run_loop(struct rec *rec, struct rmi_rec_exit *rec_exit)
{
	......
        do {
                /*
                 * We must check the status of the arch timers in every
                 * iteration of the loop to ensure we update the timer
                 * mask on each entry to the realm and that we report any
                 * change in output level to the NS caller.
                 */
                if (check_pending_timers(rec)) {
                        rec_exit->exit_reason = RMI_EXIT_IRQ;
                        break;
                }

                activate_events(rec);
                realm_exception_code = run_realm(&rec->regs[0]);
        } while (handle_realm_exit(rec, rec_exit, realm_exception_code));

        /*
         * Clear FPU/SVE and PMU context while exiting
         */
        ns_state->sve = NULL;
        ns_state->fpu = NULL;
        ns_state->pmu = NULL;

        /*
         * Clear NS pointer since that struct is local to this function.
         */
        rec->ns = NULL;

        /* Undo the heap association */
        attestation_heap_ctx_unassign_pe();
        /* Unmap auxiliary granules */
        unmap_rec_aux(rec_aux, rec->num_rec_aux);
}
```

If the exception can be handled by the RMM itself, it doesn't need to forward 
the exception to the host. However, if it needs host support to handle the 
exceptions including the RMI. As we explore the RSI, which is the synchronized 
exit from the realm through the SMC call, it will be handled by the case
ARM_EXCEPTION_SYNC_LEL.

```cpp
/* Returns 'true' when returning to Realm (S) and false when to NS */
bool handle_realm_exit(struct rec *rec, struct rmi_rec_exit *rec_exit, int exception)
{
        switch (exception) {
        case ARM_EXCEPTION_SYNC_LEL: {
                bool ret;

                /*
                 * TODO: Sanitize ESR to ensure it doesn't leak sensitive
                 * information.
                 */
                rec_exit->exit_reason = RMI_EXIT_SYNC;
                ret = handle_exception_sync(rec, rec_exit);
                if (!ret) {
                        rec->last_run_info.esr = read_esr_el2();
                        rec->last_run_info.far = read_far_el2();
                        rec->last_run_info.hpfar = read_hpfar_el2();
                }
                return ret;

                /*
                 * TODO: Much more detailed handling of exit reasons.
                 */
        }
```

It further invokes handle_exception_sync to check detailed reasons of exit from
the Realm and tries to handle the exception if possible. 

```cpp
/*
 * Return 'true' if the RMM handled the exception,
 * 'false' to return to the Non-secure host.
 */
static bool handle_exception_sync(struct rec *rec, struct rmi_rec_exit *rec_exit)
{
        const unsigned long esr = read_esr_el2();

        switch (esr & MASK(ESR_EL2_EC)) {
        case ESR_EL2_EC_WFX:
                rec_exit->esr = esr & (MASK(ESR_EL2_EC) | ESR_EL2_WFx_TI_BIT);
                advance_pc();
                return false;
        case ESR_EL2_EC_HVC:
                realm_inject_undef_abort();
                return true;
        case ESR_EL2_EC_SMC:
                if (!handle_realm_rsi(rec, rec_exit)) {
                        return false;
                }
                /*
                 * Advance PC.
                 * HCR_EL2.TSC traps execution of the SMC instruction.
                 * It is not a routing control for the SMC exception.
                 * Trap exceptions and SMC exceptions have different
                 * preferred return addresses.
                 */
                advance_pc();
                return true;
```

Because the realmn exits due to RSI which is part of the SMC, the exit will be 
handled as the ESR_EL2_EC_SMC case. 

```cpp
static bool handle_realm_rsi(struct rec *rec, struct rmi_rec_exit *rec_exit)
{
        bool ret_to_rec = true; /* Return to Realm */
        unsigned int function_id = (unsigned int)rec->regs[0];

        RSI_LOG_SET(rec->regs[1], rec->regs[2],
                    rec->regs[3], rec->regs[4], rec->regs[5]);

        /* cppcheck-suppress unsignedPositive */
        if (!IS_SMC32_PSCI_FID(function_id) && !IS_SMC64_PSCI_FID(function_id)
            && !IS_SMC64_RSI_FID(function_id)
            && !(function_id == SMCCC_VERSION)) {

                ERROR("Invalid RSI function_id = %x\n", function_id);
                rec->regs[0] = SMC_UNKNOWN;
                return true;
        }

        switch (function_id) {
	......
        case SMC_RSI_IPA_STATE_SET:
                if (handle_rsi_ipa_state_set(rec, rec_exit)) {
                        rec->regs[0] = RSI_ERROR_INPUT;
                } else {
                        advance_pc();
                        ret_to_rec = false; /* Return to Host */
                }
                break;
```
Based on the function id of the RSI it will invoke different function. In this 
case, for SMC_RSI_IPA_STATE_SET, it will invoke handle_rsi_ipa_state_set.


```cpp
bool handle_rsi_ipa_state_set(struct rec *rec, struct rmi_rec_exit *rec_exit)
{
        unsigned long start = rec->regs[1];
        unsigned long size = rec->regs[2];
        unsigned long end = start + size;
        enum ripas ripas = (enum ripas)rec->regs[3];

        if (ripas > RIPAS_RAM) {
                return true;
        }

        if (!GRANULE_ALIGNED(start)) {
                return true;
        }

        if (!GRANULE_ALIGNED(size)) {
                return true;
        }

        if (end <= start) {
                /* Size is zero, or range overflows */
                return true;
        }

        if (!region_in_rec_par(rec, start, end)) {
                return true;
        }

        rec->set_ripas.start = start;
        rec->set_ripas.end = end;
        rec->set_ripas.addr = start;
        rec->set_ripas.ripas = ripas;

        rec_exit->exit_reason = RMI_EXIT_RIPAS_CHANGE;
        rec_exit->ripas_base = start;
        rec_exit->ripas_size = size;
        rec_exit->ripas_value = (unsigned int)ripas;

        return false;
}
```
As it needs host supports to change the RIPAS through the RMI, it returns until
the smc_rec_enter function which runs realm execution loop. Note that the 
information about the ripas change is passed to the host through the rec_exit. 
Also note that similar information is also memorized in the rec->set_ripas. This
information is used later by the RMM to check if the host initiated the RMI 
properly to update the RIPAS by comparing input of the RMI and rec->set_ripas 
values. Also, note that the RMI_EXIT_RIPAS_CHANGE is set for the exit_reason 
to help host to process the RSI properly. One last thing to remember is that the 
start address is in IPA. 

```cpp
unsigned long smc_rec_enter(unsigned long rec_addr,
                            unsigned long rec_run_addr)
{
	......
        rec_run_loop(rec, &rec_run.exit);
        /* Undo the heap association */

        gic_copy_state_to_ns(&rec->sysregs.gicstate, &rec_run.exit);

out_unmap_buffers:
        buffer_unmap(rec);

        if (ret == RMI_SUCCESS) {
                if (!ns_buffer_write(SLOT_NS, g_run,
                                     offsetof(struct rmi_rec_run, exit),
                                     sizeof(struct rmi_rec_exit), &rec_run.exit)) {
                        ret = RMI_ERROR_INPUT;
                }
        }

        atomic_granule_put_release(g_rec);

        return ret;
}
```
As it needs host supports, it will returns to the host and host will process the 
rec exit due to RSI on host side.

## Host Side
As the processor enters Realm through the kvm_arch_vcpu_ioctl_run function,
let's go back to the location where the realm enter returns. 

```cpp
/**
 * kvm_arch_vcpu_ioctl_run - the main VCPU run function to execute guest code
 * @vcpu:       The VCPU pointer
 *
 * This function is called through the VCPU_RUN ioctl called from user space. It
 * will execute VM code in a loop until the time slice for the process is used
 * or some emulation is needed from user space in which case the function will
 * return with return value 0 and with the kvm_run structure filled in with the
 * required data for the requested emulation.
 */
int kvm_arch_vcpu_ioctl_run(struct kvm_vcpu *vcpu)
{
	......
        ret = 1;
        run->exit_reason = KVM_EXIT_UNKNOWN;
        run->flags = 0;
        while (ret > 0) {
		......
		/**************************************************************
                 * Enter the guest
                 */
                trace_kvm_entry(*vcpu_pc(vcpu));
                guest_timing_enter_irqoff();

                if (vcpu_is_rec(vcpu))
                        ret = kvm_rec_enter(vcpu);
                else
                        ret = kvm_arm_vcpu_enter_exit(vcpu);

                vcpu->mode = OUTSIDE_GUEST_MODE;
                vcpu->stat.exits++;
                /*
                 * Back from guest
                 *************************************************************/
		 ......
                kvm_arm_clear_debug(vcpu);

                if (vcpu_is_rec(vcpu))
                        ret = handle_rme_exit(vcpu, ret);
                else
                        ret = handle_exit(vcpu, ret);
        }
```

Note that the execution loop continues if the return value of exit handling code
is larger than zero. Below function handles the rme exit based on the reason of
exit. 

```cpp
/*
 * Return > 0 to return to guest, < 0 on error, 0 (and set exit_reason) on
 * proper exit to userspace.
 */
int handle_rme_exit(struct kvm_vcpu *vcpu, int rec_run_ret)
{
        struct rec *rec = &vcpu->arch.rec;
        u8 esr_ec = ESR_ELx_EC(rec->run->exit.esr);
        unsigned long status, index;

        status = RMI_RETURN_STATUS(rec_run_ret);
        index = RMI_RETURN_INDEX(rec_run_ret);

        /*
         * If a PSCI_SYSTEM_OFF request raced with a vcpu executing, we might
         * see the following status code and index indicating an attempt to run
         * a REC when the RD state is SYSTEM_OFF.  In this case, we just need to
         * return to user space which can deal with the system event or will try
         * to run the KVM VCPU again, at which point we will no longer attempt
         * to enter the Realm because we will have a sleep request pending on
         * the VCPU as a result of KVM's PSCI handling.
         */
        if (status == RMI_ERROR_REALM && index == 1) {
                vcpu->run->exit_reason = KVM_EXIT_UNKNOWN;
                return 0;
        }

        if (rec_run_ret)
                return -ENXIO;

        vcpu->arch.fault.esr_el2 = rec->run->exit.esr;
        vcpu->arch.fault.far_el2 = rec->run->exit.far;
        vcpu->arch.fault.hpfar_el2 = rec->run->exit.hpfar;

        update_arch_timer_irq_lines(vcpu);

        /* Reset the emulation flags for the next run of the REC */
        rec->run->entry.flags = 0;

        switch (rec->run->exit.exit_reason) {
        case RMI_EXIT_SYNC:
                return rec_exit_handlers[esr_ec](vcpu);
        case RMI_EXIT_IRQ:
        case RMI_EXIT_FIQ:
                return 1;
        case RMI_EXIT_PSCI:
                return rec_exit_psci(vcpu);
        case RMI_EXIT_RIPAS_CHANGE:
                return rec_exit_ripas_change(vcpu);
        case RMI_EXIT_HOST_CALL:
                return rec_exit_host_call(vcpu);
        }

        kvm_pr_unimpl("Unsupported exit reason: %u\n",
                      rec->run->exit.exit_reason);
        vcpu->run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
        return 0;
}
```

It handles realm exit based on the exit_reason of the realm. As we seen in the 
RMM side code, it has been set as RMI_EXIT_RIPAS_CHANGE, so it will invoke the 
exit_ripas_change function to handle the RSI.

```cpp
static int rec_exit_ripas_change(struct kvm_vcpu *vcpu)
{
        struct realm *realm = &vcpu->kvm->arch.realm;
        struct rec *rec = &vcpu->arch.rec;
        unsigned long base = rec->run->exit.ripas_base;
        unsigned long size = rec->run->exit.ripas_size;
        unsigned long ripas = rec->run->exit.ripas_value & 1;
        int ret = -EINVAL;

        if (realm_is_addr_protected(realm, base) &&
            realm_is_addr_protected(realm, base + size))
                ret = realm_set_ipa_state(vcpu, base, base + size, ripas);

        WARN(ret, "Unable to satisfy SET_IPAS for %#lx - %#lx, ripas: %#lx\n",
             base, base + size, ripas);

        return 1;
}
```

### Invoke RMI to change RIPAS
Although the RMM can change the RIPAS without returning to the host, but for 
some reasons, it asks host to invoke another RMI to change RIPAS of the page. 

```cpp
int realm_set_ipa_state(struct kvm_vcpu *vcpu,
                        unsigned long addr, unsigned long end,
                        unsigned long ripas)
{               
        int ret = 0;
             
        while (addr < end) {
                int level = find_map_level(vcpu->kvm, addr, end);
                unsigned long map_size = rme_rtt_level_mapsize(level);
                                           
                ret = set_ipa_state(vcpu, addr, addr + map_size, level, ripas);
                if (ret)
                        break;
        
                addr += map_size;
        }

        return ret;
}
```
It traverses the memory pages in the memory range specified by the rec_exit and 
invokes the RMI to ask the RMM change the RIPAS. 

```cpp
static int set_ipa_state(struct kvm_vcpu *vcpu,
                         unsigned long ipa,
                         unsigned long end,
                         int level,
                         unsigned long ripas)
{
        struct kvm *kvm = vcpu->kvm;
        struct realm *realm = &kvm->arch.realm;
        struct rec *rec = &vcpu->arch.rec;
        phys_addr_t rd_phys = virt_to_phys(realm->rd);
        phys_addr_t rec_phys = virt_to_phys(rec->rec_page);
        unsigned long map_size = rme_rtt_level_mapsize(level);
        int ret;

        while (ipa < end) {
                ret = rmi_rtt_set_ripas(rd_phys, rec_phys, ipa, level, ripas);

                if (!ret) {
                        if (!ripas)
                                kvm_realm_unmap_range(kvm, ipa, map_size);
                } else if (RMI_RETURN_STATUS(ret) == RMI_ERROR_RTT) {
                        int walk_level = RMI_RETURN_INDEX(ret);

                        if (walk_level < level) {
                                ret = realm_create_rtt_levels(realm, ipa,
                                                              walk_level,
                                                              level, NULL);
                                if (ret)
                                        return ret;
                                continue;
                        }

                        if (WARN_ON(level >= RME_RTT_MAX_LEVEL))
                                return -EINVAL;

                        /* Recurse one level lower */
                        ret = set_ipa_state(vcpu, ipa, ipa + map_size,
                                            level + 1, ripas);
                        if (ret)
                                return ret;
                } else {
                        WARN(1, "Unexpected error in %s: %#x\n", __func__,
                             ret);
                        return -EINVAL;
                }
                ipa += map_size;
        }

        return 0;
}

```

## Enter RMM to handle set_ripas RMI
As the host invokes RTT_SET_RIPAS smc, the execution control jumps to the RMM 
again, but notice that this doesn't mean that the execution goes back to the 
realm. After handling the RMI call, it will return to the host again and then 
return to the realm.

```cpp
unsigned long smc_rtt_set_ripas(unsigned long rd_addr,
                                unsigned long rec_addr,
                                unsigned long map_addr,
                                unsigned long ulevel,
                                unsigned long uripas)
{
	......
	if (ripas != rec->set_ripas.ripas) {
                ret = RMI_ERROR_INPUT;
                goto out_unmap_rec;
        }

        if (map_addr != rec->set_ripas.addr) {
                /* Target region is not next chunk of requested region */
                ret = RMI_ERROR_INPUT;
                goto out_unmap_rec;
        }
	......
        rtt_walk_lock_unlock(g_rtt_root, sl, ipa_bits,
                                map_addr, level, &wi);
        if (wi.last_level != level) {
                ret = pack_return_code(RMI_ERROR_RTT, wi.last_level);
                goto out_unlock_llt;
        }

        s2tt = granule_map(wi.g_llt, SLOT_RTT);
        s2tte = s2tte_read(&s2tt[wi.index]);

        valid = s2tte_is_valid(s2tte, level);

        if (!update_ripas(&s2tte, level, ripas)) {
                ret = pack_return_code(RMI_ERROR_RTT, (unsigned int)level);
                goto out_unmap_llt;
        }

        s2tte_write(&s2tt[wi.index], s2tte);

        if (valid && (ripas == RIPAS_EMPTY)) {
                if (level == RTT_PAGE_LEVEL) {
                        invalidate_page(&s2_ctx, map_addr);
                } else {
                        invalidate_block(&s2_ctx, map_addr);
                }
        }

        rec->set_ripas.addr += map_size;

        ret = RMI_SUCCESS;
```

Note that the rec->set_ripas which was set while handling RSI call. Because the 
ripas of realm page should not be changed freely as the request of the host, it 
compares the host provided parameter for RTT_SET_RIPAS RMI is same with the ones 
that memorized due to RSI. The most important part of this complicated procedure
is ripas can only be changed for the memory range where the realm wants to 
changes its ripas. Therefore, it checks whether the request's ripas and its 
map_addr corresponds with the request. Also note that after handling the ripas 
change, it updates the addr field of the set_ripas because the RMI is invoked 
per page, so there can be remaining RMI call to finish RSI. Now let's see how 
it actually change the ripas. 

```cpp
static bool update_ripas(unsigned long *s2tte, unsigned long level,
                         enum ripas ripas)
{
        if (s2tte_is_table(*s2tte, level)) {
                return false;
        }

        if (s2tte_is_valid(*s2tte, level)) {
                if (ripas == RIPAS_EMPTY) {
                        unsigned long pa = s2tte_pa(*s2tte, level);
                        *s2tte = s2tte_create_assigned_empty(pa, level);
                }
                return true;
        }

        if (s2tte_is_unassigned(*s2tte) || s2tte_is_assigned(*s2tte, level)) {
                *s2tte |= s2tte_create_ripas(ripas);
                return true;
        }

        return false;
}
```

Note that the ripas parameter has a new ripas value that should be updated for 
the target ipa.

```cpp
/*              
 * Returns true if @s2tte is a page or block s2tte, and NS=0.
 */     
bool s2tte_is_valid(unsigned long s2tte, long level)
{               
        return s2tte_check(s2tte, level, 0UL);
}             

static bool s2tte_check(unsigned long s2tte, long level, unsigned long ns)
{
        unsigned long desc_type;

        if ((s2tte & S2TTE_NS) != ns) {
                return false;
        }

        desc_type = s2tte & DESC_TYPE_MASK;

        /* Only pages at L3 and valid blocks at L2 allowed */
        if (((level == RTT_PAGE_LEVEL) && (desc_type == S2TTE_L3_PAGE)) ||
            ((level == RTT_MIN_BLOCK_LEVEL) && (desc_type == S2TTE_L012_BLOCK))) {
                return true;
        }

        return false;
}
```

s2tte_is_valid functions checks NS field is unset, which means that the mapped 
page is trusted ipa. Also, it validates if the mapped page is leaf page or leaf
block. If the s2tte_is_valid check passes true, it means that the page mapped by
the s2tte is trusted ipa and the leaf page. After the valid test passes, it 
checks if the requesting change is RIPAS_EMPTY. If yes, it retrieves the PA 
mapped to the existing S2TTE and generate new S2TTE with new RIPAS.


```cpp
/*
 * Creates an invalid s2tte with output address @pa, HIPAS=ASSIGNED and
 * RIPAS=EMPTY, at level @level.
 */
unsigned long s2tte_create_assigned_empty(unsigned long pa, long level)
{
        assert(level >= RTT_MIN_BLOCK_LEVEL);
        assert(addr_is_level_aligned(pa, level));
        return (pa | S2TTE_INVALID_HIPAS_ASSIGNED | S2TTE_INVALID_RIPAS_EMPTY);
}
```

Based on the request, the RIPAS is changed to S2TTE_INVALID_RIPAS_EMPTY. Note 
that the NS bit is not set for the s2tte. 

## Return to Host again from RMI
```cpp
static int set_ipa_state(struct kvm_vcpu *vcpu,
                         unsigned long ipa,
                         unsigned long end,
                         int level,
                         unsigned long ripas)
{
	......
        while (ipa < end) {
                ret = rmi_rtt_set_ripas(rd_phys, rec_phys, ipa, level, ripas);

                if (!ret) {
                        if (!ripas)
                                kvm_realm_unmap_range(kvm, ipa, map_size);

```

After returning from the rtt_set_ripas, the host should unmap the page when the 
return from the RMI success and the changed ripas was EMPTY. Because the changed
RIPAS is EMPTY not RAM, which is the result of the previous RSI call from the 
realm. If the RIPAS has been changed from the RAM to EMPTY, the page cannot be 
used for DATA page, unless RMI_RTT_SET_RIPAS(RAM) is invoked for that page.
Because realm does not want to utilize the IPA, host destroy the rtt of the ipa. 
See the two if statements. First condition checks the return value from the RMI.
If the return value was RMI_SUCCESS which is zero, then the condition is true. 
Furthermore, it checks the requested ripas to be changed, if the ripas was set 
as empty, which is zero, then the condition passes and unmap the range. 

```cpp
void kvm_realm_unmap_range(struct kvm *kvm, unsigned long ipa, u64 size)
{       
        u32 ia_bits = kvm->arch.mmu.pgt->ia_bits;
        u32 start_level = kvm->arch.mmu.pgt->start_level;
        unsigned long end = ipa + size;
        struct realm *realm = &kvm->arch.realm;
        phys_addr_t tmp_rtt = PHYS_ADDR_MAX;
        
        if (end > (1UL << ia_bits))
                end = 1UL << ia_bits;
        /*
         * Make sure we have a spare delegated page for tearing down the
         * block mappings. We must use Atomic allocations as we are called
         * with kvm->mmu_lock held.
         */
        if (realm->spare_page == PHYS_ADDR_MAX) {
                tmp_rtt = __alloc_delegated_page(realm, NULL, GFP_ATOMIC);
                /*
                 * We don't have to check the status here, as we may not
                 * have a block level mapping. Delay any error to the point
                 * where we need it.
                 */
                realm->spare_page = tmp_rtt;
        }
        
        realm_tear_down_rtt_range(&kvm->arch.realm, start_level, ipa, end);
        
        /* Free up the atomic page, if there were any */
        if (tmp_rtt != PHYS_ADDR_MAX) {
                free_delegated_page(realm, tmp_rtt);
                /*
                 * Update the spare_page after we have freed the
                 * above page to make sure it doesn't get cached
                 * in spare_page.
                 * We should re-write this part and always have
                 * a dedicated page for handling block mappings.
                 */
                realm->spare_page = PHYS_ADDR_MAX;
        }
}
```
```cpp
static int realm_tear_down_rtt_range(struct realm *realm, int level,
                                     unsigned long start, unsigned long end)
{
        phys_addr_t rd = virt_to_phys(realm->rd);
        ssize_t map_size = rme_rtt_level_mapsize(level);
        unsigned long addr, next_addr;
        bool failed = false;

        for (addr = start; addr < end; addr = next_addr) {
                phys_addr_t rtt_addr, tmp_rtt;
                struct rtt_entry rtt;
                unsigned long end_addr;

                next_addr = ALIGN(addr + 1, map_size);

                end_addr = min(next_addr, end);

                if (rmi_rtt_read_entry(rd, ALIGN_DOWN(addr, map_size),
                                       level, &rtt)) {
                        failed = true;
                        continue;
                }

                rtt_addr = rmi_rtt_get_phys(&rtt);
                WARN_ON(level != rtt.walk_level);

                switch (rtt.state) {
                case RMI_UNASSIGNED:
                case RMI_DESTROYED:
                        break;
                case RMI_TABLE:
                        if (realm_tear_down_rtt_range(realm, level + 1,
                                                      addr, end_addr)) {
                                failed = true;
                                break;
                        }
                        if (IS_ALIGNED(addr, map_size) &&
                            next_addr <= end &&
                            realm_destroy_free_rtt(realm, addr, level + 1,
                                                   rtt_addr))
                                failed = true;
                        break;
                case RMI_ASSIGNED:
                        WARN_ON(!rtt_addr);
                        /*
                         * If there is a block mapping, break it now, using the
                         * spare_page. We are sure to have a valid delegated
                         * page at spare_page before we enter here, otherwise
                         * WARN once, which will be followed by further
                         * warnings.
                         */
                        tmp_rtt = realm->spare_page;
                        if (level == 2 &&
                            !WARN_ON_ONCE(tmp_rtt == PHYS_ADDR_MAX) &&
                            realm_rtt_create(realm, addr,
                                             RME_RTT_MAX_LEVEL, tmp_rtt)) {
                                WARN_ON(1);
                                failed = true;
                                break;
                        }
                        realm_destroy_undelegate_range(realm, addr,
                                                       rtt_addr, map_size);
                        /*
                         * Collapse the last level table and make the spare page
                         * reusable again.
                         */
                        if (level == 2 &&
                            realm_rtt_destroy(realm, addr, RME_RTT_MAX_LEVEL,
                                              tmp_rtt))
                                failed = true;
                        break;
                case RMI_VALID_NS:
                        WARN_ON(rmi_rtt_unmap_unprotected(rd, addr, level));
                        break;
                default:
                        WARN_ON(1);
                        failed = true;
                        break;
                }
        }

        return failed ? -EINVAL : 0;
}
```
Host first should check the state of the target page it wants to destroy. 
rmi_rtt_read_entry function ask RMM to returns the state of the target page. 
Because the previous page requested to be changed from secure IPA to untrusted
IPA is still ASSIGNED, but empty, the state of the target page should be 
RMI_ASSIGNED. Therefore, it further undelegate and destroy page through calling 
RMIs

```cpp
static void realm_destroy_undelegate_range(struct realm *realm,
                                           unsigned long ipa,
                                           unsigned long addr,
                                           ssize_t size)
{
        unsigned long rd = virt_to_phys(realm->rd);
        int ret;                          

        while (size > 0) {
                ret = rmi_data_destroy(rd, ipa);
                WARN_ON(ret);
                ret = rmi_granule_undelegate(addr);
                                           
                if (ret)
                        get_page(phys_to_page(addr));
        
                addr += PAGE_SIZE;
                ipa += PAGE_SIZE;
                size -= PAGE_SIZE;
        }       
}       
```

Explain what is data_destory and granule undelegate.. 

## Return to RMM
After the handle_rme_exit returns (1) which means it can continue execution on
realm, it will enter the RMM through RMI_REC_ENTER RMI call. 

```cpp
unsigned long smc_rec_enter(unsigned long rec_addr,
                            unsigned long rec_run_addr)
{       
        struct granule *g_rec;
        struct granule *g_run;
        struct rec *rec;
        struct rd *rd;
        struct rmi_rec_run rec_run;
        unsigned long realm_state, ret;
        bool success;
	......
        complete_set_ripas(rec);
	......
}

static void complete_set_ripas(struct rec *rec)
{
        if (rec->set_ripas.start != rec->set_ripas.end) {
                /* Pending request from Realm */
                rec->regs[0] = RSI_SUCCESS;
                rec->regs[1] = rec->set_ripas.addr;

                rec->set_ripas.start = 0UL;
                rec->set_ripas.end = 0UL;
        }
}
```
If the start and end addresses of the set_ripas were not same, then it means 
that there were a RSI call asking ripas change before. Therefore, it complete
ripas change RSI by setting regs[0] and regs[1] and unset set_ripas fields. 
The updated regs will be fed into the realm so that the realm can confirm if 
the RSI request was processed by the host or not. Is it really safe? what if the
host enter the realm before finishing all ripas change as RSI requested? We will
see how the realm reacts to this!


## Return to realm 
Let's go back to the code right after the RSI call. 
```cpp
static inline unsigned long rsi_set_addr_range_state(phys_addr_t start,
                                                     phys_addr_t end,
                                                     enum ripas state,
                                                     phys_addr_t *top)
{
        struct arm_smccc_res res;

        invoke_rsi_fn_smc_with_res(SMC_RSI_IPA_STATE_SET,
                                   start, (end - start), state, 0, &res);

        *top = res.a1;
        return res.a0;
}       
```
As the RMM fed the rec->set_ripas.addr to the res[1] it will be passed to the 
top indicating the highest address that ripas was changed by the host and RMM. 
Also, a0 is the RSI_SUCCESS.

```cpp
static inline void set_memory_range(phys_addr_t start, phys_addr_t end,
                                    enum ripas state)
{
        unsigned long ret;
        phys_addr_t top;

        while (start != end) {
                ret = rsi_set_addr_range_state(start, end, state, &top);
                BUG_ON(ret);
                BUG_ON(top < start);
                BUG_ON(top > end);
                start = top;                         
        }                                            
}       
```
To prevent the case where the host enter the realm before finishing RSI, which 
means that RIPAS of all IPA pages requested to be changed has not been updated
properly, it checks the top address range. Also, if the start and end is not 
equal it invokes RSI again to ask host to handle it properly. If the host and 
RMM has handled the RSI request properly, it returns to __set_memory_encrypted.
Let's see what is the remaining job.

```cpp
static int __set_memory_encrypted(unsigned long addr,
                                  int numpages,
                                  bool encrypt)
{
        unsigned long set_prot = 0, clear_prot = 0;
        phys_addr_t start, end;

        if (!is_realm_world())
                return 0;

        WARN_ON(!__is_lm_address(addr));
        start = __virt_to_phys(addr);
        end = start + numpages * PAGE_SIZE;

        if (encrypt) {
                clear_prot = PROT_NS_SHARED;
                set_memory_range_protected(start, end);
        } else {
                set_prot = PROT_NS_SHARED;
                set_memory_range_shared(start, end);
        }

        return __change_memory_common(addr, PAGE_SIZE * numpages,
                                      __pgprot(set_prot),
                                      __pgprot(clear_prot));
}
```

As the RIPAS has been changed by the RSI, the IPA should be changed as well. 
Remind that the MSB of the IPA is used to distinguish whether it is mapped to
trusted or non-trusted IPA. This change on IPA is done by the below function. 

```cpp
/*
 * This function assumes that the range is mapped with PAGE_SIZE pages.
 */
static int __change_memory_common(unsigned long start, unsigned long size,
                                pgprot_t set_mask, pgprot_t clear_mask)
{
        struct page_change_data data;
        int ret;

        data.set_mask = set_mask;
        data.clear_mask = clear_mask;

        ret = apply_to_page_range(&init_mm, start, size, change_page_range,
                                        &data);

        flush_tlb_kernel_range(start, start + size);
        return ret;
}
```

The apply_to_page_range function invokes the passed function, changed_page_range,
for the ptep of the memory range specified by the start and size. Note that the 
start address is the virtual, so this macro will invoke this function with the 
pte of each page mapped in between [start, start+size]. Also note that it passes
the set_mask and clear_mask as data to the function. If we assume that the 
previous RSI changed the RIPAS of the IPA pages to the RIPAS_EMPTY, the set_prot
field is set as PROT_NS_SHARED, which is the MSB of the IPA. Remember that the 
trusted and untrusted IPA is split into lower and upper half utilizing MSB of 
the IPA. 


```cpp
static int change_page_range(pte_t *ptep, unsigned long addr, void *data)
{
        struct page_change_data *cdata = data;
        pte_t pte = READ_ONCE(*ptep);

        pte = clear_pte_bit(pte, cdata->clear_mask);
        pte = set_pte_bit(pte, cdata->set_mask);

        /* TODO: Break before make for PROT_NS_SHARED updates */
        set_pte(ptep, pte);
        return 0;
}
```

If the realm wants to change the trusted IPA to untrusted, then its IPA should
be mapped to upper half, and this is accomplished by set_pte_bit macro. It sets
MSB of the pte and store it in the pte. The set_pte function will update the 
pte. As the virtual address used for DMA is not mapped from the trusted to 
untrusted IPA by updating the PTE, which means updating the IPA, the accesses 
through this memory address will trigger the data abort exit from the realm. 
This is because the untrusted IPA is not mapped to host physical address in the 
s2tt. Therefore, the raised fault should be handled by the RMM and the s2tt 
should be patched accordingly to make the realm access the untrusted host memory.


## Mapping untrusted IPA to the page 
How the fault raised in the realm due to accessing untrusted can be resolved? 
After change_page_range is invoked for the DMA range, the IPA originally used 
for the DMA is moved to the upper half by changing stage 1 page table of the 
realm. However, accessing this page from the realm through the virtual address
should generate the fault, cause there is no s2tt mapping. That is what I 
described about how the realm allocates untrusted IPA for DMA. Then how the host
handles the fault generated due to accessing it? 

### Revisit host for handling fault-ipa
When we try to remind of how the host handles the fault_ipa, first it checks if 
there is any memslot translating the fault-ipa (IPA) to HVA. If not, it is 
treated as MMIO and first handled by the kernel. If it cannot be handled by the 
kernel itself, it exits to the user and ask handling the MMIO. However, if there
is a memslot for the faultin IPA, then the KVM treats the fault-ipa as non-MMIO
and tries to generate mapping from the faultin IPA to HPA because memslot can 
be utilized to retrieve HPA from HVA mapped to the IPA.

Then what happens after the trusted IPA is translated into untrusted IPA after 
the RSI to reserve untrusted DMA space for realm? Note that the faultin ipa is 
not exactly same as before because the MSB is set because the realm changes the 
stage 1 page table. Yeah there is a potential chance that this changed IPA could 
be treated as MMIO address if the entire address including the MSB is used to 
locate the memslot for the IPA. Intuitively, we can assume that the IPA without
the MSB would be used make the realm treat it as non-mmio address to generate 
proper mapping :) But let's to be clear by checking the code!


```cpp
int kvm_handle_guest_abort(struct kvm_vcpu *vcpu)
{               
        unsigned long fault_status;
        phys_addr_t fault_ipa, fault_ipa_stolen;
        struct kvm_memory_slot *memslot;
        unsigned long hva;
        bool is_iabt, write_fault, writable;
        gpa_t gpa_stolen_mask = kvm_gpa_stolen_bits(vcpu->kvm);
        gfn_t gfn;
        int ret, idx;

        fault_status = kvm_vcpu_trap_get_fault_type(vcpu);

	//1
        fault_ipa = kvm_vcpu_get_fault_ipa(vcpu);
	......

	//2
	gfn = (fault_ipa & ~gpa_stolen_mask) >> PAGE_SHIFT;
        memslot = gfn_to_memslot(vcpu->kvm, gfn);
        hva = gfn_to_hva_memslot_prot(memslot, gfn, &writable);
        write_fault = kvm_is_write_fault(vcpu);

	if (kvm_is_error_hva(hva) || (write_fault && !writable)) {
		......
		//3
		fault_ipa |= kvm_vcpu_get_hfar(vcpu) & ((1 << 12) - 1);
                fault_ipa &= ~gpa_stolen_mask;
                ret = io_mem_abort(vcpu, fault_ipa);
                goto out_unlock;
        }

	......
	//4
	ret = user_mem_abort(vcpu, fault_ipa, memslot, hva, fault_status);
	......
}
```

There are four points that are relevant to faultin ipa as shown in the code and 
comments. Basically the fault_ipa field is the ipa as it is without masking out 
the MSB. Therefore, by checking fault_ipa we can know whether it is untrusted 
or trusted IPA. When you look at the second point, the gfn is retrieved from the 
fault_ipa by masking out the trusted/untrusted bit. Therefore, whether the
faultin IPA is in trusted or untrusted, gfn will be same if the other bits
except the MSB are identical. Note that this masked out IPA is used to search 
memslot mapping the gfn to hva. If there is an memslot, then the if condition
checking kvm_is_error_hva will return false, which means it is not the address
requiring MMIO emulation. 

This is important. Remind that the fault happen after the ripas change to 
allocate untrusted DMA memory. In other words, this indicate that there is a 
memslot translating the gfn to hva because the gfn was previously mapped to the 
trusted IPA. Regardless of MSB indicating the trusted or untrusted for IPA, if 
the IPA has been mapped to the trusted originally, then there should be a 
memslot for it! If the KVM haven't deleted this memslot while destroying the 
page through RMI, the memslot should exist. Therefore, the IPA which was mapped 
to trusted before, but currently mapped to untrusted can be treated as non-MMIO
memory, and its fault will be handled by user_mem_abort not by the io_mem_abort. 

Also note that the fault_ipa itself it passed to the user_mem_abort without 
masking out the MSB. However, before invoking the io_mem_abort, it masks out the 
MSB from the fault_ipa. It is easy to find the reason when you think about it 
carefully. io_mem_abort needs the IPA to check if it is within MMIO range and 
it doesn't need exact IPA including the trusted/untrusted bit because the actual
mapping between the fault_ipa to HPA will not be established as a result. 
However, if the faultin_ipa is non-MMIO address, then the faultin IPA should be 
mapped in the stage 2 page table to allow the realm access untrusted IPA. 
Therefore, it needs entire IPA including the MSB is necessary. If the IPA is 
passed to the user_mem_abort after masking out the MSB, then it will establish
mapping from trusted IPA to HPA which will not be accessed by the realm. 


## Questions & Answers
### How to enforce access control on RIPAS==EMPTY?
>Realm data access to a Protected IPA whose RIPAS is EMPTY causes a Synchronous
>External Abort taken to the Realm.
realm_destroy_undelegate_range)





### prot_ns_shared bit

```cpp
void __init arm64_rsi_init(void)
{
        if (!rsi_version_matches())
                return;
        if (rsi_get_realm_config(&config))
                return;
        prot_ns_shared = BIT(config.ipa_bits - 1);

        if (config.ipa_bits - 1 < phys_mask_shift)
                phys_mask_shift = config.ipa_bits - 1;

        static_branch_enable(&rsi_present);
}
```

This field has been initialized by the above function to indicate which bit is 
used to split the address space in guest VM. Therefore, for the case where the 
memory has been set as decrypted, the set_mask of the data is set with the bit 
determining the upper half. 
