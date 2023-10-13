### KVM side handling
```cpp
int io_mem_abort(struct kvm_vcpu *vcpu, phys_addr_t fault_ipa)
{       
        struct kvm_run *run = vcpu->run;
        unsigned long data;
        unsigned long rt;
        int ret;
        bool is_write;
        int len;
        u8 data_buf[8];
        
        /*
         * No valid syndrome? Ask userspace for help if it has
         * volunteered to do so, and bail out otherwise.
         */
        if (!kvm_vcpu_dabt_isvalid(vcpu)) {
                if (test_bit(KVM_ARCH_FLAG_RETURN_NISV_IO_ABORT_TO_USER,
                             &vcpu->kvm->arch.flags)) {
                        run->exit_reason = KVM_EXIT_ARM_NISV;
                        run->arm_nisv.esr_iss = kvm_vcpu_dabt_iss_nisv_sanitized(vcpu);
                        run->arm_nisv.fault_ipa = fault_ipa;
                        return 0;
                }
                
                kvm_pr_unimpl("Data abort outside memslots with no valid syndrome info\n");
                return -ENOSYS;
        }
        
        /*
         * Prepare MMIO operation. First decode the syndrome data we get
         * from the CPU. Then try if some in-kernel emulation feels
         * responsible, otherwise let user space do its magic.
         */
        is_write = kvm_vcpu_dabt_iswrite(vcpu);
        len = kvm_vcpu_dabt_get_as(vcpu);
        rt = kvm_vcpu_dabt_get_rd(vcpu);
        
        //this is where the mmio is emulated by the kvm..
        if (is_write) {
                data = vcpu_data_guest_to_host(vcpu, vcpu_get_reg(vcpu, rt),
                                               len);
                
                trace_kvm_mmio(KVM_TRACE_MMIO_WRITE, len, fault_ipa, &data);
                kvm_mmio_write_buf(data_buf, len, data);
                
                ret = kvm_io_bus_write(vcpu, KVM_MMIO_BUS, fault_ipa, len,
                                       data_buf);
        } else {
                trace_kvm_mmio(KVM_TRACE_MMIO_READ_UNSATISFIED, len,
                               fault_ipa, NULL);
                
                ret = kvm_io_bus_read(vcpu, KVM_MMIO_BUS, fault_ipa, len,
                                      data_buf);
        }
        
        /* Now prepare kvm_run for the potential return to userland. */
        run->mmio.is_write      = is_write;
        run->mmio.phys_addr     = fault_ipa;
        run->mmio.len           = len;
        vcpu->mmio_needed       = 1;
        
        if (vcpu_is_rec(vcpu))
                vcpu->arch.rec.run->entry.flags |= RMI_EMULATED_MMIO;
        
        if (!ret) {
		/* We handled the access successfully in the kernel. */
                printk("[MMIO KERNEL]: fault_ipa:%llx\n", fault_ipa);
                if (!is_write)
                        memcpy(run->mmio.data, data_buf, len);
                vcpu->stat.mmio_exit_kernel++;
                kvm_handle_mmio_return(vcpu);
                return 1;
        }
        
	//user needs to be involved to handle mmio
        if (is_write)
                memcpy(run->mmio.data, data_buf, len);
        vcpu->stat.mmio_exit_user++;
        run->exit_reason        = KVM_EXIT_MMIO;
        return 0;
}
```

The mmio exit events can be handled in two different places: KVM side or user 
application utilizing KVM module. Therefore, the kernel function handling the 
MMIO first tries to resolve the MMIO request (triggered due to the fault) within
the VM first and then exit to the user if it cannot handle it. Regarding the CCA
it sets the RMI_EMULATED_MMIO flags in the rec_run_entry so that the RMM can 
reflect the processed MMIO emulation result to the VM state. 

### Kernel MMIO handling 

### User MMIO handling 

### Resolve MMIO and update vcpu for realm (RMM side)
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
	/*
         * Check GIC state after checking other conditions but before doing
         * anything which may have side effects.
         */
        gic_copy_state_from_ns(&rec->sysregs.gicstate, &rec_run.entry);
        if (!gic_validate_state(&rec->sysregs.gicstate)) {
                ret = RMI_ERROR_REC;
                goto out_unmap_buffers;
        }

        if (!complete_mmio_emulation(rec, &rec_run.entry)) {
                ret = RMI_ERROR_REC;
                goto out_unmap_buffers;
        }

        if (!complete_sea_insertion(rec, &rec_run.entry)) {
                ret = RMI_ERROR_REC;
                goto out_unmap_buffers;
        }
	.....
}
```
Every realm entrance, RMM checks if the mmio emulation is accomplished by the 
host. 



```cpp
#define REC_ENTRY_FLAG_EMUL_MMIO      (UL(1) << 0)
static bool complete_mmio_emulation(struct rec *rec, struct rmi_rec_entry *rec_entry)
{
        unsigned long esr = rec->last_run_info.esr;
        unsigned int rt = esr_srt(esr);

        if ((rec_entry->flags & REC_ENTRY_FLAG_EMUL_MMIO) == 0UL) {
                return true;
        }

        if (((esr & MASK(ESR_EL2_EC)) != ESR_EL2_EC_DATA_ABORT) ||
            !(esr & ESR_EL2_ABORT_ISV_BIT)) {
                /*
                 * MMIO emulation is requested but the REC did not exit with
                 * an emulatable exit.
                 */
                return false;
        }

        /*
         * Emulate mmio read (unless the load is to xzr)
         */
        if (!esr_is_write(esr) && (rt != 31U)) {
                unsigned long val;

                val = rec_entry->gprs[0] & access_mask(esr);

                if (esr_sign_extend(esr)) {
                        unsigned int bit_count = access_len(esr) * 8U;
                        unsigned long mask = 1UL << (bit_count - 1U);

                        val = (val ^ mask) - mask;
                        if (!esr_sixty_four(esr)) {
                                val &= (1UL << 32U) - 1UL;
                        }
                }

                rec->regs[rt] = val;
        }

        rec->pc = rec->pc + 4UL;
        return true;
}
```

Because rec enter always checks if there is a pending MMIO emulation should be 
reflected to the realm, it first checks if REC_ENTRY_FLAG_EMUL_MMIO flag was set
by the host. Most of the case it will not be set, and the function returns true
to continue realm execution. However, if the host sets the flag, then the RMM 
should check if the realm did exit due to the emulatable data abort by checking 
the last_run_info. This is to prevent host from maliciously feeding input to the
realm.



