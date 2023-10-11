## Host Side
```cpp
int kvm_arch_vcpu_ioctl_run(struct kvm_vcpu *vcpu)
{
        struct kvm_run *run = vcpu->run;
        int ret;

        if (run->exit_reason == KVM_EXIT_MMIO) {
                ret = kvm_handle_mmio_return(vcpu);
                if (ret)
                        return ret;
        }

        vcpu_load(vcpu);

        if (run->immediate_exit) {
                ret = -EINTR;
                goto out;
        }

        kvm_sigset_activate(vcpu);

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
                if (vcpu_is_rec(vcpu))
                        ret = handle_rme_exit(vcpu, ret);
                else
                        ret = handle_exit(vcpu, ret);
        }
```



```cpp
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


```cpp
static exit_handler_fn rec_exit_handlers[] = {
        [0 ... ESR_ELx_EC_MAX]  = rec_exit_reason_notimpl,
        [ESR_ELx_EC_SYS64]      = rec_exit_sys_reg,
        [ESR_ELx_EC_DABT_LOW]   = rec_exit_sync_dabt,
        [ESR_ELx_EC_IABT_LOW]   = rec_exit_sync_iabt
};


static int rec_exit_sync_dabt(struct kvm_vcpu *vcpu)
{
        struct rec *rec = &vcpu->arch.rec;

        if (kvm_vcpu_dabt_iswrite(vcpu) && kvm_vcpu_dabt_isvalid(vcpu))
                vcpu_set_reg(vcpu, kvm_vcpu_dabt_get_rd(vcpu),
                             rec->run->exit.gprs[0]);

        return kvm_handle_guest_abort(vcpu);
}


```


```cpp
/**
 * kvm_handle_guest_abort - handles all 2nd stage aborts
 * @vcpu:       the VCPU pointer
 *
 * Any abort that gets to the host is almost guaranteed to be caused by a
 * missing second stage translation table entry, which can mean that either the
 * guest simply needs more memory and we must allocate an appropriate page or it
 * can mean that the guest tried to access I/O memory, which is emulated by user
 * space. The distinction is based on the IPA causing the fault and whether this
 * memory region has been registered as standard RAM by user space.
 */
int kvm_handle_guest_abort(struct kvm_vcpu *vcpu)
{
        unsigned long fault_status;
        phys_addr_t fault_ipa;
        struct kvm_memory_slot *memslot;
        unsigned long hva;
        bool is_iabt, write_fault, writable;
        gpa_t gpa_stolen_mask = kvm_gpa_stolen_bits(vcpu->kvm);
        gfn_t gfn;
        int ret, idx;
......
        ret = user_mem_abort(vcpu, fault_ipa, memslot, hva, fault_status);
        if (ret == 0)
                ret = 1;
out:
        if (ret == -ENOEXEC) {
                kvm_inject_pabt(vcpu, kvm_vcpu_get_hfar(vcpu));
                ret = 1;
        }
out_unlock:
        srcu_read_unlock(&vcpu->kvm->srcu, idx);
        return ret;
}
```


```cpp
static int user_mem_abort(struct kvm_vcpu *vcpu, phys_addr_t fault_ipa,
                          struct kvm_memory_slot *memslot, unsigned long hva,
                          unsigned long fault_status)
{
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
```

## Map different pages based on faultin ipa!
As the IPA space is split into two (trusted and untrusted), based on where the 
fault raised, the host should invoke different RMI to map page to the realm. If
the faultin IPA is untrusted, it means that the faultin IPA space has been set 
as the request of the REALM, which was done by the RSI_IPA_STATE_SET. Also, 
while processing this RSI in the host side, the address mapped to the currently
faultin address was unmapped. Moreover, after returning from the RSI, the REALM
has changed its mapping to untrusted IPA from the trusted. As this address was 
unmapped, note that this unmapping can be done because the IPA was changed from
trusted to unrtused before, it will raise the fault from the changed untrusted 
IPA. Therefore, now it is the time to map faultin IPA to the untrusted address. 

```cpp
static int realm_map_ipa(struct kvm *kvm,  ipa, unsigned long hva,phys_addr_e
                         kvm_pfn_t pfn, unsigned long map_size,
                         enum kvm_pgtable_prot prot,
                         struct kvm_mmu_memory_cache *memcache)
{
        struct realm *realm = &kvm->arch.realm;
        struct page *page = pfn_to_page(pfn);

        if (WARN_ON(!(prot & KVM_PGTABLE_PROT_W)))
                return -EFAULT;

        if (!realm_is_addr_protected(realm, ipa))
                return realm_map_non_secure(realm, ipa, page, map_size,
                                            memcache);

        return realm_map_protected(realm, hva, ipa, page, map_size, memcache);
}
```

Host can also easily tell which IPA the faultin address belongs to because 
untrusted API is mapped to upper half of the IPA and the trusted is mapped to 
the lower half. 

```cpp
static inline bool realm_is_addr_protected(struct realm *realm,
                                           unsigned long addr)
{       
        unsigned int ia_bits = realm->ia_bits;
        
        return !(addr & ~(BIT(ia_bits - 1) - 1));
}       
```


```cpp
#define S2TTE_ATTRS     (S2TTE_MEMATTR_FWB_NORMAL_WB | S2TTE_AP_RW | \
                        S2TTE_SH_IS | S2TTE_AF)

#define S2TTE_PAGE      (S2TTE_ATTRS | S2TTE_L3_PAGE)
#define S2TTE_PAGE_NS   (S2TTE_NS | S2TTE_XN | S2TTE_AF | S2TTE_L3_PAGE)
```

The biggest difference between secure IPA valid page and ns IPA valid page is 
NS bit. Also, the shareability and writeback settings are enforced for secure
IPA pages. \TODO{need to check what are the guarantees of these three flags 
S2TTE_MEMATTR_FWB_NORMAL_WB, S2TTE_AP_RW, and S2TTE_SH_IS for the secure IPA}.


### Map to untrusted IPA
```cpp
int realm_map_non_secure(struct realm *realm,
                         unsigned long ipa,
                         struct page *page,
                         unsigned long map_size,
                         struct kvm_mmu_memory_cache *memcache)
{
        phys_addr_t rd = virt_to_phys(realm->rd);
        int map_level;
        int ret = 0;
        unsigned long desc = page_to_phys(page) |
                             PTE_S2_MEMATTR(MT_S2_FWB_NORMAL) |
                             /* FIXME: Read+Write permissions for now */
                             (3 << 6) |
                             PTE_SHARED;

        if (WARN_ON(!IS_ALIGNED(ipa, map_size)))
                return -EINVAL;

        switch (map_size) {
        case PAGE_SIZE:
                map_level = 3;
                break;
        case RME_L2_BLOCK_SIZE:
                map_level = 2;
                break;
        default:
                return -EINVAL;
        }

        ret = rmi_rtt_map_unprotected(rd, ipa, map_level, desc);

        if (RMI_RETURN_STATUS(ret) == RMI_ERROR_RTT) {
                /* Create missing RTTs and retry */
                int level = RMI_RETURN_INDEX(ret);

                ret = realm_create_rtt_levels(realm, ipa, level, map_level,
                                              memcache);
                if (WARN_ON(ret))
                        return -ENXIO;

                ret = rmi_rtt_map_unprotected(rd, ipa, map_level, desc);
        }
        if (WARN_ON(ret))
                return -ENXIO;

        return 0;
}
```

### Map to trusted IPA 
```cpp
int realm_map_protected(struct realm *realm,
                        unsigned long hva,
                        unsigned long base_ipa,
                        struct page *dst_page,
                        unsigned long map_size,
                        struct kvm_mmu_memory_cache *memcache)
{
        phys_addr_t dst_phys = page_to_phys(dst_page);
        phys_addr_t rd = virt_to_phys(realm->rd);
        unsigned long phys = dst_phys;
        unsigned long ipa = base_ipa;
        unsigned long size;
        int map_level;
        int ret = 0;

        if (WARN_ON(!IS_ALIGNED(ipa, map_size)))
                return -EINVAL;

        switch (map_size) {
        case PAGE_SIZE:
                map_level = 3;
                break;
        case RME_L2_BLOCK_SIZE:
                map_level = 2;
                break;
        default:
                return -EINVAL;
        }

        if (map_level < RME_RTT_MAX_LEVEL) {
                /*
                 * A temporary RTT is needed during the map, precreate it,
                 * however if there is an error (e.g. missing parent tables)
                 * this will be handled below.
                 */
                realm_create_rtt_levels(realm, ipa, map_level,
                                        RME_RTT_MAX_LEVEL, memcache);
        }

        for (size = 0; size < map_size; size += PAGE_SIZE) {
                if (rmi_granule_delegate(phys)) {
                        struct rtt_entry rtt;

                        /*
                         * It's possible we raced with another VCPU on the same
                         * fault. If the entry exists and matches then exit
                         * early and assume the other VCPU will handle the
                         * mapping.
                         */
                        if (rmi_rtt_read_entry(rd, ipa, RME_RTT_MAX_LEVEL, &rtt))
                                goto err;

                        // FIXME: For a block mapping this could race at level
                        // 2 or 3...
                        if (WARN_ON((rtt.walk_level != RME_RTT_MAX_LEVEL ||
                                     rtt.state != RMI_ASSIGNED ||
                                     rtt.desc != phys))) {
                                goto err;
                        }

                        return 0;
                }

                ret = rmi_data_create_unknown(phys, rd, ipa);

                if (RMI_RETURN_STATUS(ret) == RMI_ERROR_RTT) {
                        /* Create missing RTTs and retry */
                        int level = RMI_RETURN_INDEX(ret);

                        ret = realm_create_rtt_levels(realm, ipa, level,
                                                      RME_RTT_MAX_LEVEL,
                                                      memcache);
                        WARN_ON(ret);
                        if (ret)
                                goto err_undelegate;

                        ret = rmi_data_create_unknown(phys, rd, ipa);
                }
                WARN_ON(ret);

                if (ret)
                        goto err_undelegate;

                phys += PAGE_SIZE;
                ipa += PAGE_SIZE;
        }

        if (map_size == RME_L2_BLOCK_SIZE)
                ret = fold_rtt(rd, base_ipa, map_level, realm);
        if (WARN_ON(ret))
                goto err;

        return 0;

err_undelegate:
        if (WARN_ON(rmi_granule_undelegate(phys))) {
                /* Page can't be returned to NS world so is lost */
                get_page(phys_to_page(phys));
        }
err:
        while (size > 0) {
                phys -= PAGE_SIZE;
                size -= PAGE_SIZE;
                ipa -= PAGE_SIZE;

                rmi_data_destroy(rd, ipa);

                if (WARN_ON(rmi_granule_undelegate(phys))) {
                        /* Page can't be returned to NS world so is lost */
                        get_page(phys_to_page(phys));
                }
        }
        return -ENXIO;
}
```
The most noticeable difference compared with generating untrusted ipa mapping is
that it requires the physical page that will be mapped should be delegated to 
the realm before calling RMI to generate mapping to that physical page. Also it 
utilize RMI SMC_RMM_DATA_CREATE_UNKNOWN instead of SMC_RMM_RTT_MAP_UNPROTECTED 
to generate mapping in the RTT. 
