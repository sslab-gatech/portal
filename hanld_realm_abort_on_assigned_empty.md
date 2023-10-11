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
	......
}                                                                               
```    

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

### Inject synchronous data abort
```cpp
/*
 * Inject the Synchronous Instruction or Data Abort into the current REC.
 * The I/DFSC field in the ESR_EL1 is set to @fsc
 */
void inject_sync_idabort(unsigned long fsc)
{               
        unsigned long esr_el2 = read_esr_el2();
        unsigned long far_el2 = read_far_el2();
        unsigned long elr_el2 = read_elr_el2();
        unsigned long spsr_el2 = read_spsr_el2();
        unsigned long vbar_el2 = read_vbar_el12();

        unsigned long esr_el1 = calc_esr_idabort(esr_el2, spsr_el2, fsc);
        unsigned long pc = calc_vector_entry(vbar_el2, spsr_el2);
        unsigned long pstate = calc_pstate();

                
        write_far_el12(far_el2); //faulting virtual address to guest
        write_elr_el12(elr_el2); //return address to guest
        write_spsr_el12(spsr_el2); //Saved Program Status Register
        write_esr_el12(esr_el1); //Exception Syndrome Register
        write_elr_el2(pc); //Exception Link Register 
        write_spsr_el2(pstate);
}               
```
The most important register set by the above function is elr_el2. When taking an
exception to EL2, this register holds the address to return to. As the RMM is 
EL2, when it returns from the RMM to the realm, processor jumps to the address
pointed to by this register. Therefore, updating this register means that RMM 
redirect the program counter of the realm to new location. As the injected fault
should be hanlded by the realm, RMM updates this register as the vector function 
address. 


### Calculate vector entry in the realm 
```cpp
/*
 * Calculate the address of the vector entry when an exception is inserted
 * into the Realm.
 *
 * @vbar The base address of the vector table in the Realm.
 * @spsr The Saved Program Status Register at EL2.
 */     
static unsigned long calc_vector_entry(unsigned long vbar, unsigned long spsr)
{       
        unsigned long offset; 
        
        if ((spsr & MASK(SPSR_EL2_MODE)) == SPSR_EL2_MODE_EL1h) {
                offset = VBAR_CEL_SP_ELx_OFFSET;
        } else if ((spsr & MASK(SPSR_EL2_MODE)) == SPSR_EL2_MODE_EL1t) {
                offset = VBAR_CEL_SP_EL0_OFFSET;
        } else if ((spsr & MASK(SPSR_EL2_MODE)) == SPSR_EL2_MODE_EL0t) {
                if ((spsr & MASK(SPSR_EL2_nRW)) == SPSR_EL2_nRW_AARCH64) {
                        offset = VBAR_LEL_AA64_OFFSET;
                } else {
                        offset = VBAR_LEL_AA32_OFFSET;
                }
        } else {
                assert(false);
                offset = 0UL;
        }
        
        return vbar + offset;
}
```

The first register passed to this function is vbar which holds the vector base
address of the realm for any exception. Wait, When you look at the read_XXX and 
write_XX macro, there are EL12 and EL2. Meaning of EL2 is clear, RMM. However,
what is EL12? EL12 is encoding that access EL1 system register when FEAT_VHE is 
implemented and HCR_EL2.E2H is 1. Therefore, based on the mode (SPSR[3:0]) which
presents exception level and selected stack pointer, the offset will be 
determined and added to the vbar register to generate address of the handler 
stored in the vector table. 

### Calculate exception reason
```cpp
/*
 * Calculate the content of the Realm's esr_el1 register when
 * the Synchronous Instruction or Data Abort is injected into
 * the Realm (EL1).
 *
 * The value is constructed from the @esr_el2 & @spsr_el2 that
 * are captured when the exception from the Realm was taken to EL2.
 *
 * The fault status code (ESR_EL1.I/DFSC) is set to @fsc
 */
static unsigned long calc_esr_idabort(unsigned long esr_el2,
                                      unsigned long spsr_el2,
                                      unsigned long fsc)
{
        /*
         * Copy esr_el2 into esr_el1 apart from the following fields:
         * - The exception class (EC). Its value depends on whether the
         *   exception to EL2 was from either EL1 or EL0.
         * - I/DFSC. It will be set to @fsc.
         * - FnV. It will set to zero.
         * - S1PTW. It will be set to zero. 
         */     
        unsigned long esr_el1 = esr_el2 & ~(MASK(ESR_EL2_EC)        |
                                            MASK(ESR_EL2_ABORT_FSC) |
                                            ESR_EL2_ABORT_FNV_BIT   |

        unsigned long ec = esr_el2 & MASK(ESR_EL2_EC);

        assert((ec == ESR_EL2_EC_INST_ABORT) || (ec == ESR_EL2_EC_DATA_ABORT));
        if ((spsr_el2 & MASK(SPSR_EL2_MODE)) != SPSR_EL2_MODE_EL0t) {
                ec += 1UL << ESR_EL2_EC_SHIFT;
        }
        esr_el1 |= ec;

        /*
         * Set the I/DFSC.
         */
        assert((fsc & ~MASK(ESR_EL2_ABORT_FSC)) == 0UL);
        esr_el1 |= fsc;

        /*
         * Set the EA.
         */
        esr_el1 |= ESR_EL2_ABORT_EA_BIT;

        return esr_el1;
}
```

The most important part of calculating exception is setting I/DFSC part because 
it indicates detailed exception and guide realm OS to interpret exception 
injected from the RMM properly. Note that I/DFSC of syndrome register will be
set as ESR_EL2_ABORT_FSC_SEA which is 0x10

## Handling injected exception from realm
```cpp
int kvm_handle_guest_abort(struct kvm_vcpu *vcpu)
{           
        /* Synchronous External Abort? */
        if (kvm_vcpu_abt_issea(vcpu)) {
                /*
                 * For RAS the host kernel may handle this abort.
                 * There is no need to pass the error into the guest.
                 */
                if (kvm_handle_guest_sea(fault_ipa, kvm_vcpu_get_esr(vcpu)))
                        kvm_inject_vabt(vcpu);

                return 1;
        }
```


