Let's assume that the realm exits due to data abort specifically because of the 
unmapped page access. In that case, the realm needs the page mapping to the 
faultin ipa in the s2tt. Let's see how this mapping is established focusing on 
the interactions between the RMM and host. 

```cpp
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
        case ESR_EL2_EC_SYSREG: {
                bool ret = handle_sysreg_access_trap(rec, rec_exit, esr);

                advance_pc();
                return ret;
        }
        case ESR_EL2_EC_INST_ABORT:
                return handle_instruction_abort(rec, rec_exit, esr);
        case ESR_EL2_EC_DATA_ABORT:
                return handle_data_abort(rec, rec_exit, esr);
```

Because the data abort is treated as a synchronous exception, it will be handled
by the handle_exception_sync function. Also, based on the ESR_El2_EC bit of the 
esr_el2 register, we can know if the realm exits due to data abort. As a 
continue of the previous posting regarding how the realm map dma through RIPAS
change, we assume that the realm accesses the untrusted IPA , which raise the 
data abort fault and exit the realm.


```cpp
static bool handle_data_abort(struct rec *rec, struct rmi_rec_exit *rec_exit,
                              unsigned long esr)
{
        unsigned long far = 0UL;
        unsigned long hpfar = read_hpfar_el2();
        unsigned long fipa = (hpfar & MASK(HPFAR_EL2_FIPA)) << HPFAR_EL2_FIPA_OFFSET;
        unsigned long write_val = 0UL;

        if (handle_sync_external_abort(rec, rec_exit, esr)) {
                INFO("SEEMS THAT NEVER HAPPEN\n");
                /*
                 * All external aborts are immediately reported to the host.
                 */
                return false;
        }

        /*
         * The memory access that crosses a page boundary may cause two aborts
         * with `hpfar_el2` values referring to two consecutive pages.
         *
         * Insert the SEA and return to the Realm if the granule's RIPAS is EMPTY.
         */
        if (ipa_is_empty(fipa, rec)) {
                inject_sync_idabort(ESR_EL2_ABORT_FSC_SEA);
                return true;
        }

        if (fixup_aarch32_data_abort(rec, &esr) ||
            access_in_rec_par(rec, fipa)) {
                esr &= ESR_NONEMULATED_ABORT_MASK;
                goto end;
        }

        if (esr_is_write(esr)) {
                write_val = get_dabt_write_value(rec, esr);
        }

        far = read_far_el2() & ~GRANULE_MASK;
        esr &= ESR_EMULATED_ABORT_MASK;

end:
        rec_exit->esr = esr;
        rec_exit->far = far;
        rec_exit->hpfar = hpfar;
        rec_exit->gprs[0] = write_val;

        return false;
}
```


### Check faultin IPA is EMPTY or Untrusted IPA

```cpp
static bool ipa_is_empty(unsigned long ipa, struct rec *rec)
{               
        unsigned long s2tte, *ll_table;
        struct rtt_walk wi;
        enum ripas ripas;
        bool ret;
                
        assert(GRANULE_ALIGNED(ipa));
                
        if (!addr_in_rec_par(rec, ipa)) {
                return false;
        }
        granule_lock(rec->realm_info.g_rtt, GRANULE_STATE_RTT);
        
        rtt_walk_lock_unlock(rec->realm_info.g_rtt,
                             rec->realm_info.s2_starting_level,
                             rec->realm_info.ipa_bits,
                             ipa, RTT_PAGE_LEVEL, &wi);
                
        ll_table = granule_map(wi.g_llt, SLOT_RTT);
        s2tte = s2tte_read(&ll_table[wi.index]);

        if (s2tte_is_destroyed(s2tte)) {
                ret = false;
                goto out_unmap_ll_table;
        }       
        ripas = s2tte_get_ripas(s2tte);
        ret = (ripas == RIPAS_EMPTY);
                        
out_unmap_ll_table:
        buffer_unmap(ll_table);
        granule_unlock(wi.g_llt);
        return ret;
}
```
It checks two important conditions. First and foremost, it checks if the IPA is
Trusted or Untrusted (addr_in_rec_par check). For example, if the fault happens
due to accessing DMA memory at the first time before the S2TTE mapping is 
established yet, it will return false. This is intuitive to return false because 
there is no RIPAS for untrusted IPA.

If faultin address is within the Trusted IPA, it walks the s2tt and locate the
last entry in the RTT utilized to map the faultin IPA to HPA. After the walking,
it validates if the RIPAS of the S2TTE is empty or not. If it turns out that 
the S2TTE RIPAS is EMPTY, it enters to the realm after injection SEA fault to it
instead of rec_exit. 

```cpp
static bool handle_data_abort(struct rec *rec, struct rmi_rec_exit *rec_exit,
                              unsigned long esr)
{
	......
        far = read_far_el2() & ~GRANULE_MASK;
        esr &= ESR_EMULATED_ABORT_MASK;

end:
        rec_exit->esr = esr;
        rec_exit->far = far;
        rec_exit->hpfar = hpfar;
        rec_exit->gprs[0] = write_val;

        return false;
}

```

After the RIPAS check, it updates rec_exit to provide information of the fault 
to the host because host should invoke proper RMI to generate mapping in RTT. 
Also note that the exit_reason was set as RMI_EXIT_SYNC for data abort in the 
handle_realm_exit function. Let's see how the host handle the fault.


## Host side
We already cover most of the relevant functions to handle the data abort in host
on previous posting [[]]. I will only cover the relevant functions that will map
untrusted IPA to host physical address. 

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

As the untrusted IPA has not been mapped through the s2tt, there should be no 
s2tt entries in the RMM, which makes the RMI_RTT_MAP_UNPROTECTED return error. 
As a fall-through, it invokes realm_create_rtt_levels to establish all mappings
required for the faultin-ipa and then invoke the RTT_MAP_UNPROTECTED RMI once 
again. 

For the RMM details see [[]].

