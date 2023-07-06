## RMM Initialization (tf-a side)

### Initialize context and entry point info
std_svc_setup -> rmmd_setup -> bl31_register_rmm_init(&rmm_init)


### Setup execution context of processor for RMM
bl31_main -> rmm_init -> rmm_context[plat_my_core_pos()] (each core can have different RMM context)
                      -> rmm_el2_context_init
                      -> rmmd_rmm_sync_entry -> cm_set_context (set REALM context)
             	                             -> rmmd_rmm_enter -> el3_exit

Because processor requires different contexts for different world, the tf-a 
should maintain the processor context for each world. Therefore tf-a should 
generate RMM context for all cores in the platform. Only one core will invoke 
the bl31_main function and subsequent functions associated with initializing 
cores for RMM. Other remaining cores will be initialized and have unique context
when they are boot-up. 

### Structure used to store context of different worlds 
```cpp
/*
 * Top-level context structure which is used by EL3 firmware to preserve
 * the state of a core at the next lower EL in a given security state and
 * save enough EL3 meta data to be able to return to that EL and security
 * state. The context management library will be used to ensure that
 * SP_EL3 always points to an instance of this structure at exception
 * entry and exit.
 */
typedef struct cpu_context {
        gp_regs_t gpregs_ctx;
        el3_state_t el3state_ctx;
        el1_sysregs_t el1_sysregs_ctx;
#if CTX_INCLUDE_EL2_REGS
        el2_sysregs_t el2_sysregs_ctx;
#endif
#if CTX_INCLUDE_FPREGS
        fp_regs_t fpregs_ctx;
#endif
        cve_2018_3639_t cve_2018_3639_ctx;
#if CTX_INCLUDE_PAUTH_REGS
        pauth_t pauth_ctx;
#endif
} cpu_context_t;
```

### Initialize remaining cores for RMM
```cpp
SUBSCRIBE_TO_EVENT(psci_cpu_on_finish, rmmd_cpu_on_finish_handler); 
```

When the CPU boots up it invokes the rmmd_cpu_on_finish_handler function to make
the processor enters the rmm so that the processors are initialized to enter 
RMM layer if necessary. 

rmmd_cpu_on_finish_handler -> rmmd_rmm_sync_entry


### Enter to the RMM
```cpp
func rmmd_rmm_enter
        /* Make space for the registers that we're going to save */
        mov     x3, sp
        str     x3, [x0, #0]
        sub     sp, sp, #RMMD_C_RT_CTX_SIZE

        /* Save callee-saved registers on to the stack */
        stp     x19, x20, [sp, #RMMD_C_RT_CTX_X19]
        stp     x21, x22, [sp, #RMMD_C_RT_CTX_X21]
        stp     x23, x24, [sp, #RMMD_C_RT_CTX_X23]
        stp     x25, x26, [sp, #RMMD_C_RT_CTX_X25]
        stp     x27, x28, [sp, #RMMD_C_RT_CTX_X27]
        stp     x29, x30, [sp, #RMMD_C_RT_CTX_X29]

        /* ---------------------------------------------------------------------
         * Everything is setup now. el3_exit() will use the secure context to
         * restore to the general purpose and EL3 system registers to ERET
         * into the secure payload.
         * ---------------------------------------------------------------------
         */
        b       el3_exit
endfunc rmmd_rmm_enter
```

## Cold boot (RMM code side)

### Call stacks for cold boot
rmm_entry -> plat_setup (uart setup and initialize xlat table)
                  -> plat_cmn_setup -> rmm_el3_ifc_init (initialize RMM<->Monitor shared buffer) / (setup static_regions to indicate which regions comprise of RMM)
		                    -> xlat_ctx_cfg_init
		                    -> xlat_ctx_init ->  xlat_init_tables_ctx -> xlat_tables_map_region (map regions in the xlat table)
				    -> gic_get_virt_features
				    -> slot_buf_coldboot_init -> xlat_ctx_cfg_init (data structures required to map slot buffers at warm boot are prepared) 
                  -> plat_warmboot_setup (see [[]])
          -> xlat_enable_mmu_el2 (enable mmu by setting SCTLR_EL2)
	  -> rmm_main
	  -> smc_ret

### Platform setup for coldboot

```cpp
IMPORT_SYM(uintptr_t, rmm_text_start, RMM_CODE_START);
IMPORT_SYM(uintptr_t, rmm_text_end, RMM_CODE_END);
IMPORT_SYM(uintptr_t, rmm_ro_start, RMM_RO_START);
IMPORT_SYM(uintptr_t, rmm_ro_end, RMM_RO_END);
IMPORT_SYM(uintptr_t, rmm_rw_start, RMM_RW_START);
IMPORT_SYM(uintptr_t, rmm_rw_end, RMM_RW_END);

/*
 * Memory map REGIONS used for the RMM runtime (static mappings)
 */
#define RMM_CODE_SIZE           (RMM_CODE_END - RMM_CODE_START)
#define RMM_RO_SIZE             (RMM_RO_END - RMM_RO_START)
#define RMM_RW_SIZE             (RMM_RW_END - RMM_RW_START)

#define RMM_CODE                MAP_REGION_FLAT(                        \
                                        RMM_CODE_START,                 \
                                        RMM_CODE_SIZE,                  \
                                        MT_CODE | MT_REALM)

#define RMM_RO                  MAP_REGION_FLAT(                        \
                                        RMM_RO_START,                   \
                                        RMM_RO_SIZE,                    \
                                        MT_RO_DATA | MT_REALM)

#define RMM_RW                  MAP_REGION_FLAT(                        \
                                        RMM_RW_START,                   \
                                        RMM_RW_SIZE,                    \
                                        MT_RW_DATA | MT_REALM)

        /* Common regions sorted by ascending VA */
        struct xlat_mmap_region regions[COMMON_REGIONS] = {
                RMM_CODE,
                RMM_RO,
                RMM_RW,
                RMM_SHARED
        };
```
The regions array define memory regions that should be mapped by the mmu before 
enter the RMM main function. Here, the macro defined the 1:1 va to pa mapping. 

### Set-up xlat table and associated data structures 
```cpp
/* Struct that holds the context configuration */
struct xlat_ctx_cfg {
        /*
         * Maximum size allowed for the VA space handled by the context.
         */
        uintptr_t max_va_size;

        /*
         * Pointer to an array with all the memory regions stored in order
         * of ascending base_va.
         */
        struct xlat_mmap_region *mmap;

        /*
         * Number of regions stored in the mmap array.
         */
        unsigned int mmap_regions;

        /*
         * Base address for the virtual space on this context.
         */
        uintptr_t base_va;

        /*
         * Max Physical and Virtual addresses currently in use by the
         * current context. These will get updated as we map memory
         * regions but it will never go beyond the maximum physical address
         * or max_va_size respectively.
         *
         * max_mapped_pa is an absolute Physical Address.
         */
        uintptr_t max_mapped_pa;
        uintptr_t max_mapped_va_offset;

        /* Level of the base translation table. */
	unsigned int base_level;

        /*
         * Virtual address region handled by this context.
         */
        xlat_addr_region_id_t region;

        bool initialized;
};
```
This data structure holds all information about the translation table and the 
regions that will be mapped by the translation table. Because it is very basic 
step of the cold boot, so it will just map the only necessary regions to run 
RMM code with its data and some shared buffers. 


```cpp
/*                                      
 * Struct that holds the context itself, composed of
 * a pointer to the context config and a pointer to the
 * translation tables associated to it.
 */
struct xlat_ctx {
        struct xlat_ctx_cfg *cfg;
        struct xlat_ctx_tbls *tbls;
};
```

This structure is utilized by the xlat_init_tables_ctx and xlat_tables_map_region
function to actually map the regions through setting the tables translated by 
the mmu. 


## Warm boot
rmm_entry -> plat_warmboot_setup -> plat_cmn_warmboot_setup -> xlat_arch_setup_mmu_cfg (setup mair_el2, tcr_el2, ttbr_el2)
                                                            -> slot_buf_setup_xlat -> xlat_ctx_init
							                           -> xlat_arch_setup_mmu_cfg
          -> xlat_enable_mmu_el2 (setup sctlr_el2 register to enable mmu)
	  -> rmm_warmboot_main -> rmm_arch_init
	                       -> slot_buf_finish_warmboot_init
          -> smc_ret





### TCR_EL2, Translation Control Register (EL2)
### MAIR_EL2, Memory Attribute Indirection Register (EL2)


### Set-up caches for ttbr1 last table entries
```cpp
/*
 * Finishes initializing the slot buffer mechanism.
 * This function must be called after the MMU is enabled.
 */
void slot_buf_finish_warmboot_init(void)
{
        assert(is_mmu_enabled() == true);

        /*
         * Initialize (if not done yet) the internal cache with the last level
         * translation table that holds the MMU descriptors for the slot
         * buffers, so we can access them faster when we need to map/unmap.
         */
        if ((get_cached_llt_info())->table == NULL) {
                if (xlat_get_llt_from_va(get_cached_llt_info(),
                                         get_slot_buf_xlat_ctx(),
                                         slot_to_va(SLOT_NS)) != 0) {
                        ERROR("%s (%u): Failed to initialize table entry cache for CPU %u\n",
                                        __func__, __LINE__, my_cpuid());
                        panic();

                }
        }
}
```


### Return to the tf-a
As the last part of the init function of the RMM, it jumps to the smc_ret label,
which invokes the smc#0 instruction. This invokes the tf-a and handles the smc 
call following function orders. 

```cpp
sync_exception_aarch64 -> handle_sync_exception -> sync_handler64 -> std_svc_smc_handler -> rmmd_rmm_el3_handler -> rmmd_rmm_sync_exit
                                                                  -> el3_exit (to rmm)
```


runtime_exceptions is the exception table registered for EL3. When the SMC call
is invoked from the user side, it is interpreted as an exception to the EL3. 
Therefore, EL3 invokes proper exception handler stored in the runtime_exceptions.
That function is the sync_exception_aarch64 and it invokes the proper exception
handler registered for servicing SMC call. To handle the SMC calls in tf-a, the 
service handler and its init functions should be registered through the macro,
DECLARE_RT_SVC. When the interrupt to EL3 happens due to SMC and the SMC call 
number matches with the service number that should be handled by the registered
service, it invokes the registered handler function, which is std_svc_smc_handler 
function in this case. When you look into the smc_handler64 function, it 
retrieves the address of the handler registered for the current SMC number. 
Also, std_svc_setup function in the tf-a initialize all the registered service
during the service initialization. 



```cpp
/*******************************************************************************
 * This function returns to the place where rmmd_rmm_sync_entry() was
 * called originally.
 ******************************************************************************/
__dead2 void rmmd_rmm_sync_exit(uint64_t rc)
{
        rmmd_rmm_context_t *ctx = &rmm_context[plat_my_core_pos()];

        /* Get context of the RMM in use by this CPU. */
        assert(cm_get_context(REALM) == &(ctx->cpu_ctx));

        /*
         * The RMMD must have initiated the original request through a
         * synchronous entry into RMM. Jump back to the original C runtime
         * context with the value of rc in x0;
         */
        rmmd_rmm_exit(ctx->c_rt_ctx, rc);

        panic();
}
```

```cpp
func rmmd_rmm_exit
        /* Restore the previous stack */
        mov     sp, x0

        /* Restore callee-saved registers on to the stack */
        ldp     x19, x20, [x0, #(RMMD_C_RT_CTX_X19 - RMMD_C_RT_CTX_SIZE)]
        ldp     x21, x22, [x0, #(RMMD_C_RT_CTX_X21 - RMMD_C_RT_CTX_SIZE)]
        ldp     x23, x24, [x0, #(RMMD_C_RT_CTX_X23 - RMMD_C_RT_CTX_SIZE)]
        ldp     x25, x26, [x0, #(RMMD_C_RT_CTX_X25 - RMMD_C_RT_CTX_SIZE)]
        ldp     x27, x28, [x0, #(RMMD_C_RT_CTX_X27 - RMMD_C_RT_CTX_SIZE)]
        ldp     x29, x30, [x0, #(RMMD_C_RT_CTX_X29 - RMMD_C_RT_CTX_SIZE)]

        /* ---------------------------------------------------------------------
         * This should take us back to the instruction after the call to the
         * last rmmd_rmm_enter().* Place the second parameter to x0
         * so that the caller will see it as a return value from the original
         * entry call.
         * ---------------------------------------------------------------------
         */
        mov     x0, x1
        ret
endfunc rmmd_rmm_exit
```

After the execution of the rmmd_rmm_sync_exit, it returns all the way up to the 
sync_handler64 function. After the return it continues the execution and jump 
to the el3_exit assembly function to exit from el3. 

```cpp
func el3_exit
#if ENABLE_ASSERTIONS
        /* el3_exit assumes SP_EL0 on entry */
        mrs     x17, spsel
        cmp     x17, #MODE_SP_EL0
        ASM_ASSERT(eq)
#endif /* ENABLE_ASSERTIONS */

        /* ----------------------------------------------------------
         * Save the current SP_EL0 i.e. the EL3 runtime stack which
         * will be used for handling the next SMC.
         * Then switch to SP_EL3.
         * ----------------------------------------------------------
         */
        mov     x17, sp
        msr     spsel, #MODE_SP_ELX
        str     x17, [sp, #CTX_EL3STATE_OFFSET + CTX_RUNTIME_SP]

#if IMAGE_BL31
        /* ----------------------------------------------------------
         * Restore CPTR_EL3.
         * ZCR is only restored if SVE is supported and enabled.
         * Synchronization is required before zcr_el3 is addressed.
         * ----------------------------------------------------------
         */
        ldp     x19, x20, [sp, #CTX_EL3STATE_OFFSET + CTX_CPTR_EL3]
        msr     cptr_el3, x19

        ands    x19, x19, #CPTR_EZ_BIT
        beq     sve_not_enabled

        isb
        msr     S3_6_C1_C2_0, x20 /* zcr_el3 */
sve_not_enabled:
#endif /* IMAGE_BL31 */

#if IMAGE_BL31 && DYNAMIC_WORKAROUND_CVE_2018_3639
        /* ----------------------------------------------------------
         * Restore mitigation state as it was on entry to EL3
         * ----------------------------------------------------------
         */
        ldr     x17, [sp, #CTX_CVE_2018_3639_OFFSET + CTX_CVE_2018_3639_DISABLE]
        cbz     x17, 1f
        blr     x17
1:
#endif /* IMAGE_BL31 && DYNAMIC_WORKAROUND_CVE_2018_3639 */

#if IMAGE_BL31 && RAS_EXTENSION
        /* ----------------------------------------------------------
         * Issue Error Synchronization Barrier to synchronize SErrors
         * before exiting EL3. We're running with EAs unmasked, so
         * any synchronized errors would be taken immediately;
         * therefore no need to inspect DISR_EL1 register.
         * ----------------------------------------------------------
         */
        esb
#else
        dsb     sy
#endif /* IMAGE_BL31 && RAS_EXTENSION */

        /* ----------------------------------------------------------
         * Restore SPSR_EL3, ELR_EL3 and SCR_EL3 prior to ERET
         * ----------------------------------------------------------
         */
        ldr     x18, [sp, #CTX_EL3STATE_OFFSET + CTX_SCR_EL3]
        ldp     x16, x17, [sp, #CTX_EL3STATE_OFFSET + CTX_SPSR_EL3]
        msr     scr_el3, x18
        msr     spsr_el3, x16
        msr     elr_el3, x17

        restore_ptw_el1_sys_regs

        /* ----------------------------------------------------------
         * Restore general purpose (including x30), PMCR_EL0 and
         * ARMv8.3-PAuth registers.
         * Exit EL3 via ERET to a lower exception level.
         * ----------------------------------------------------------
         */
        bl      restore_gp_pmcr_pauth_regs
        ldr     x30, [sp, #CTX_GPREGS_OFFSET + CTX_GPREG_LR]

#ifdef IMAGE_BL31
        str     xzr, [sp, #CTX_EL3STATE_OFFSET + CTX_IS_IN_EL3]
#endif /* IMAGE_BL31 */

        exception_return

endfunc el3_exit


```


## Enter the RMM from the tf-a


std_svc_smc_handler -> rmmd_rmi_handler -> rmmd_smc_forward (from src -> dst) -> 
cm_set_next_eret_context -> cm_set_next_context (bl1_next_cpu_context_ptr = context) ->
SMC_RETX (to setup the registers for the return context)

Compared to initialization phase, because already all contexts are created, it 
can easily return to the context of that world by simply changing the registers. 
rmmd_smc_forward knows where the SMC call should be forwarded into. As the TF-A
maintains all context for different worlds, incoming context can be saved into 
the context maintained in the tf-a. Also, it should read the source state from
the memory and load them to processor, preparing to return to the destination 
world. SMC_RETX macro set up the registers for the destination context so that 
it can return to that context when it exits the tf-a.

### The last part of the RMM 
```cpp
func rmm_entry

        rmm_el2_init_env el2_vectors, cold_boot_flag, skip_to_warmboot

        /*
         * Initialize platform specific peripherals like UART and
         * xlat tables.
         */
        bl      plat_setup
        bl      xlat_enable_mmu_el2

        bl      rmm_main
        b       smc_ret

skip_to_warmboot:
        /*
         * Carry on with the rest of the RMM warmboot path
         */
        bl      plat_warmboot_setup
        bl      xlat_enable_mmu_el2

        bl      rmm_warmboot_main
smc_ret:
        mov_imm x0, SMC_RMM_BOOT_COMPLETE
        mov_imm x1, E_RMM_BOOT_SUCCESS
        smc     #0

        /* Jump to the SMC handler post-init */
        b       rmm_handler
```

The processor exit from RMM to tf-a through smc call. Therefore, the processor
context for RMM should point to the next instruction of the SMC call, which 
will make the processor jump into rmm_handler when it continues its execution 
from the RMM. 


### RMM handler loop for NS SMC 
```cpp
func rmm_handler
        /*
         * Save Link Register and X4, as per SMCCC v1.2 its value
         * must be preserved unless it contains result, as specified
         * in the function definition.
         */
        stp     x4, lr, [sp, #-16]!

        /*
         * Zero the space for X0-X3 in the smc_result structure
         * and pass its address as the last argument.
         */
        stp     xzr, xzr, [sp, #-16]!
        stp     xzr, xzr, [sp, #-16]!
        mov     x7, sp

        bl      handle_ns_smc

        /*
         * Copy command output values back to caller. Since this is
         * done through SMC, X0 is used as the FID, and X1-X5 contain
         * the values of X0-X4 copied from the smc_result structure.
         */
        ldr     x0, =SMC_RMM_REQ_COMPLETE
        ldp     x1, x2, [sp], #16
        ldp     x3, x4, [sp], #16
        ldp     x5, lr, [sp], #16

        smc     #0

        /* Continue the rmm handling loop */
        b       rmm_handler
endfunc rmm_handler
```

All communication from the NS to RMM is done through the rmm_handler function.
After the RMM initialization, this function is invoked whenever the RMM should 
process the SMC generated from the NS. As shown in the code, at the end of the 
rmm_handler function it jumps to the rmm_handler again following the smc #0 
instruction. It means that the processor context always starts from right after
the smc #0 instruction and jump to the rmm_handler to process another SMC call
for RMM. Actual SMC call handle is done by the handle_ns_smc function. 



## SMC call handling in RMM 
```cpp
void handle_ns_smc(unsigned long function_id,
                   unsigned long arg0,
                   unsigned long arg1,
                   unsigned long arg2,
                   unsigned long arg3,
                   unsigned long arg4,
                   unsigned long arg5,
                   struct smc_result *ret)
{               
        unsigned long handler_id;
        const struct smc_handler *handler = NULL;
                
        if (IS_SMC64_RMI_FID(function_id)) {
                handler_id = SMC_RMI_HANDLER_ID(function_id);
                if (handler_id < ARRAY_LEN(smc_handlers)) {
                        handler = &smc_handlers[handler_id];
                }
        }
        
        /*      
         * Check if handler exists and 'fn_dummy' is not NULL
         * for not implemented 'function_id' calls in SMC RMI range.
         */     
        if ((handler == NULL) || (handler->fn_dummy == NULL)) {
                VERBOSE("[%s] unknown function_id: %lx\n",
                        __func__, function_id);
                ret->x[0] = SMC_UNKNOWN;
                return;
        }
                
        assert_cpu_slots_empty();
        
        switch (handler->type) {
        case rmi_type_0:
                ret->x[0] = handler->f0();
                break;
        case rmi_type_1:
                ret->x[0] = handler->f1(arg0);
                break;
        case rmi_type_2:
                ret->x[0] = handler->f2(arg0, arg1);
                break;
        case rmi_type_3:
                ret->x[0] = handler->f3(arg0, arg1, arg2);
                break;
        case rmi_type_4:
                ret->x[0] = handler->f4(arg0, arg1, arg2, arg3);
                break;
        case rmi_type_5:
                ret->x[0] = handler->f5(arg0, arg1, arg2, arg3, arg4);
                break;
        case rmi_type_1_o:
                handler->f1_o(arg0, ret);
                break;
        case rmi_type_3_o:
                handler->f3_o(arg0, arg1, arg2, ret);
                break;
        default:
                assert(false);
        }
        
        if (rmi_call_log_enabled) {
                rmi_log_on_exit(handler_id, arg0, arg1, arg2, arg3, arg4, ret);
        }

        assert_cpu_slots_empty();
}
```

### List of SMC handled by the RMM
```cpp
static const struct smc_handler smc_handlers[] = {
        HANDLER_0(SMC_RMM_VERSION,               smc_version,                   true,  true),
        HANDLER_1_O(SMC_RMM_FEATURES,            smc_read_feature_register,     true,  true, 1U),
        HANDLER_1(SMC_RMM_GRANULE_DELEGATE,      smc_granule_delegate,          false, true),
        HANDLER_1(SMC_RMM_GRANULE_UNDELEGATE,    smc_granule_undelegate,        false, true),
        HANDLER_2(SMC_RMM_REALM_CREATE,          smc_realm_create,              true,  true),
        HANDLER_1(SMC_RMM_REALM_DESTROY,         smc_realm_destroy,             true,  true),
        HANDLER_1(SMC_RMM_REALM_ACTIVATE,        smc_realm_activate,            true,  true),
        HANDLER_3(SMC_RMM_REC_CREATE,            smc_rec_create,                true,  true),
        HANDLER_1(SMC_RMM_REC_DESTROY,           smc_rec_destroy,               true,  true),
        HANDLER_2(SMC_RMM_REC_ENTER,             smc_rec_enter,                 false, true),
        HANDLER_5(SMC_RMM_DATA_CREATE,           smc_data_create,               false, false),
        HANDLER_3(SMC_RMM_DATA_CREATE_UNKNOWN,   smc_data_create_unknown,       false, false),
        HANDLER_2(SMC_RMM_DATA_DESTROY,          smc_data_destroy,              false, true),
        HANDLER_4(SMC_RMM_RTT_CREATE,            smc_rtt_create,                false, true),
        HANDLER_4(SMC_RMM_RTT_DESTROY,           smc_rtt_destroy,               false, true),
        HANDLER_4(SMC_RMM_RTT_FOLD,              smc_rtt_fold,                  false, true),
        HANDLER_4(SMC_RMM_RTT_MAP_UNPROTECTED,   smc_rtt_map_unprotected,       false, false),
        HANDLER_3(SMC_RMM_RTT_UNMAP_UNPROTECTED, smc_rtt_unmap_unprotected,     false, false),
        HANDLER_3_O(SMC_RMM_RTT_READ_ENTRY,      smc_rtt_read_entry,            false, true, 4U),
        HANDLER_2(SMC_RMM_PSCI_COMPLETE,         smc_psci_complete,             true,  true),
        HANDLER_1_O(SMC_RMM_REC_AUX_COUNT,       smc_rec_aux_count,             true,  true, 1U),
        HANDLER_3(SMC_RMM_RTT_INIT_RIPAS,        smc_rtt_init_ripas,            false, true),
        HANDLER_5(SMC_RMM_RTT_SET_RIPAS,         smc_rtt_set_ripas,             false, true)
};

```



## Granules in RMM and tf-a
Granules enforced by the hardware are managed by the tf-a, but the RMM also 
maintains the mirror of the granule so that it can handles SMC calls related 
with the granules without communicating to the tf-a every time. 

### RMM maintained granule array
```cpp
struct granule {
        /*
         * @lock protects the struct granule itself. Take this lock whenever
         * inspecting or modifying any other fields in this struct.
         */
        spinlock_t lock;

        /*
         * @state is the state of the granule.
         */
        enum granule_state state;

        /*
         * @refcount counts RMM and realm references to this granule with the
         * following rules:
         *  - The @state of the granule cannot be modified when @refcount
         *    is non-zero.
         *  - When a granule is mapped into the RMM, either the granule lock
         *    must be held or a reference must be held.
         *  - The content of the granule itself can be modified when
         *    @refcount is non-zero without holding @lock.  However, specific
         *    types of granules may impose further restrictions on concurrent
         *    access.
         */
        unsigned long refcount;
};
```

For all physical DRAM page, RMM maintains associated granule structure entry.
The main purpose of maintaining this additional information in addition to 
actual GPT is to enforce additional security guarantees to the CCA VMs. From 
the tf-a's perspective, all pages delegated to the RMM is equal, but the RMM 
utilize the delegated pages with different purposes. As the RMM layer is kind of
a micro-code of the processor, the guarantee provided by the tf-rmm is very 
strong as if it is enforced by the hardware (although it can be more easily 
breached compared to the layer closed to the actual hardware). Below enum 
granule_state showcases what I mention. 

```cpp
enum granule_state {
        /*
         * Non-Secure granule (external)
         *
         * Granule content is not protected by granule::lock, as it is always
         * subject to reads and writes from the NS world.
         */
        GRANULE_STATE_NS,
        /*
         * TODO: remove the next line when spec aligment is done
         * currently this has been renamed in alp03 and is needed for CBMC testbench
         */
        GRANULE_STATE_UNDELEGATED = GRANULE_STATE_NS,
        /*
         * Delegated Granule (external)
         *
         * Granule content is protected by granule::lock.
         *
         * No references are held on this granule type.
         */
        GRANULE_STATE_DELEGATED,
        /*
         * Realm Descriptor Granule (external)
         *
         * Granule content is protected by granule::lock.
         *
         * A reference is held on this granule:
         * - For each associated REC granule.
         *
         * The RD may only be destroyed when the following objects
         * have a reference count of zero:
         * - The root-level RTT
         */
        GRANULE_STATE_RD,
        /*
         * Realm Execution Context Granule (external)
         *
         * Granule content (see struct rec) comprises execution
         * context state and cached realm information copied from the RD.
         *
         * Execution context is not protected by granule::lock, because we can't
         * enter a Realm while holding the lock.
         *
         * The following rules with respect to the granule's reference apply:
         * - A reference is held on this granule when a REC is running.
         * - As REC cannot be run on two PEs at the same time, the maximum
         *   value of the reference count is one.
         * - When the REC in entered, the reference count is incremented
         *   (set to 1) atomically while granule::lock is held.
         * - When the REC exits, the reference counter is released (set to 0)
         *   atomically with store-release semantics without granule::lock being
         *   held.
         * - The RMM can access the granule's content on the entry and exit path
         *   from the REC while the reference is held.
         */
        GRANULE_STATE_REC,
        /*
         * Realm Execution Context auxiliary granule (internal)
         *
         * Granule auxiliary content is used to store any state that cannot
         * fit in the main REC page. This is typically used for context
         * save/restore of PE features like SVE, SME, etc.
         *
         * Granule content is not protected by granule::lock nor the reference
         * count. The RMM can access the content of the auxiliary granules
         * only while holding a lock or reference to the parent REC granule.
         *
         * The granule::lock is held during a state change to
         * GRANULE_STATE_REC_AUX and from GRANULE_STATE_REC_AUX.
         *
         * The complete internal locking order when changing REC_AUX
         * granule's state is:
         *
         * REC -> REC_AUX[0] -> REC_AUX[1] -> ... -> REC_AUX[n-1]
         */
        GRANULE_STATE_REC_AUX,

        /*
         * Data Granule (internal)
         *
         * Granule content is not protected by granule::lock, as it is always
         * subject to reads and writes from within a Realm.
         *
         * A granule in this state is always referenced from exactly one entry
         * in an RTT granule which must be locked before locking this granule.
         * Only a single DATA granule can be locked at a time.
         * The complete internal locking order for DATA granules is:
         *
         * RD -> RTT -> RTT -> ... -> DATA
         *
         * No references are held on this granule type.
         */
        GRANULE_STATE_DATA,
        /*
         * RTT Granule (internal)
         *
         * Granule content is protected by granule::lock.
         *
         * Granule content is protected by granule::lock, but hardware
         * translation table walks may read the RTT at any point in time.
         * TODO: do we wish/need to use hardware access flag management?
         *
         * Multiple granules in this state can only be locked at the same time
         * if they are part of the same tree, and only in topological order
         * from root to leaf. The topological order of concatenated root level
         * RTTs is from lowest address to highest address.
         *
         * The complete internal locking order for RTT granules is:
         *
         * RD -> [RTT] -> ... -> RTT
         *
         * A reference is held on this granule for each entry in the RTT that
         * refers to a granule:
         *   - Table s2tte.
         *   - Valid s2tte.
         *   - Valid_NS s2tte.
         *   - Assigned s2tte.
         */
        GRANULE_STATE_RTT,
        GRANULE_STATE_LAST = GRANULE_STATE_RTT
};
```
There should be NS PAS or REALM PAS page in terms of GPT, but the RMM layer can
utilize software concept to distinguish one page based on its usage. 



## Translation in RMM
There are two memory regions translated by the different ttbr0 and ttbr1 in the
RMM layer. See the below code for initialzing two different contexts.

```cpp
        ret = xlat_ctx_cfg_init(&runtime_xlat_ctx_cfg, VA_LOW_REGION,
                                &static_regions[0], nregions + COMMON_REGIONS,
                                VIRT_ADDR_SPACE_SIZE);

        return xlat_ctx_cfg_init(&slot_buf_xlat_ctx_cfg, VA_HIGH_REGION,
                                 &slot_buf_regions[0], 1U,
                                 RMM_SLOT_BUF_VA_SIZE);
```

```cpp
        ret = xlat_ctx_init(&runtime_xlat_ctx, &runtime_xlat_ctx_cfg,
                            &runtime_tbls,
                            &static_s1tt[0],
                            PLAT_CMN_CTX_MAX_XLAT_TABLES);

	//slot buf region is per processor region
	struct xlat_ctx *slot_buf_ctx = get_slot_buf_xlat_ctx();
        int ret = xlat_ctx_init(slot_buf_ctx,
                                &slot_buf_xlat_ctx_cfg,
                                &slot_buf_tbls[cpuid],
                                &slot_buf_s1tt[XLAT_TABLE_ENTRIES * cpuid], 1U);
```

```cpp
        /* Setup the MMU cfg for the low region (runtime context) */
        ret = xlat_arch_setup_mmu_cfg(&runtime_xlat_ctx);

        if (xlat_arch_setup_mmu_cfg(get_slot_buf_xlat_ctx())) {
                ERROR("%s (%u): MMU registers failed to initialize\n",
                                        __func__, __LINE__);
                panic();
        }
```

### Reading/Writing from/to normal world memory in RMM
To simplify the mapping in the RMM, it utilize cached slots (fixed virtual addr)
to map physical pages temporarily. For example, there are handful of pages that 
needs to be mapped in the RMM to set-up (e.g., memzero or memcpy) so each page 
type has fixed address slot that is used for any physical page mapping. This
usage is very short and not-frequent, instead of giving another virtual addr to 
every other physical page, it temporarily maps the page to always fixed same 
virtual address, which allows fast mapping. For this, RMM maintains cache of the
page table associated with those virtual address instead of walking entire page 
table for every page mapping. 

ns_granule_map -> buffer_arch_map(buffer_map_internal) -> xlat_map_memory_page_with_attrs -> ns_buffer_unmap

```cpp
void *buffer_map_internal(enum buffer_slot slot, unsigned long addr)
{                                   
        uint64_t attr = SLOT_DESC_ATTR;
        uintptr_t va = slot_to_va(slot);
        struct xlat_llt_info *entry = get_cached_llt_info();
        
        assert(GRANULE_ALIGNED(addr));

        attr |= (slot == SLOT_NS ? MT_NS : MT_REALM);

        if (xlat_map_memory_page_with_attrs(entry, va,
                                            (uintptr_t)addr, attr) != 0) {
                /* Error mapping the buffer */
                return NULL;
        }

        return (void *)va;
}               
```

As shown in the above function, to retrieve the VA, it just passes the slot 
indicating which page it is used for. The buffer has one slot per each different
purposed page that is always mapped to different physical page dynamically. 
Also, RMM maintains the last entries of the translation tables as a cache, so 
instead of walking entire translation table to map the PA to designated VA, it 
retrieves the table entries from the cache (get_cached_llt_info). With these 
two information, it invokes xlat_map_memory_page_with_attrs function that maps
the provided VA to PA utilizing the provided table information. 

xlat_map_memory_page_with_attrs -> xlat_get_tte_ptr (retrieve leaf entry)
                                -> xlat_desc (generate new leaf descriptor)
                                -> xlat_write_tte (write to the table)

To conclude, RMM layer has capabilities to map any physical pages to any VA, but
the mapping the granule pages are done by very limited interface but done in 
very fast way. 


## SMC_RMM_REALM_CREATE





## SMC_RMM_REC_CREATE
RMM should be able to access normal world memory to copy the nw provided data 
structures or pass the information to the host. To this end, RMM defines two 
apis ns_buffer_read and ns_buffer_write function to provide read or write access
for the RMM layer to the NW memory. Internally both functions invokes the 
ns_granule_map function to map the NS page. 


```cpp
unsigned long smc_rec_create(unsigned long rec_addr,
                             unsigned long rd_addr,
                             unsigned long rec_params_addr)
{
        struct granule *g_rd;
        struct granule *g_rec;
        struct granule *rec_aux_granules[MAX_REC_AUX_GRANULES];
        struct granule *g_rec_params;
        struct rec *rec;
        struct rd *rd;
        struct rmi_rec_params rec_params;
        unsigned long rec_idx;
        enum granule_state new_rec_state = GRANULE_STATE_DELEGATED;
        unsigned long ret;
        bool ns_access_ok;
        unsigned int num_rec_aux;

        g_rec_params = find_granule(rec_params_addr);
        if ((g_rec_params == NULL) || (g_rec_params->state != GRANULE_STATE_NS)) {
                return RMI_ERROR_INPUT;
        }
        
        ns_access_ok = ns_buffer_read(SLOT_NS, g_rec_params, 0U,
                                      sizeof(rec_params), &rec_params);
```
For example, smc_rec_create handler for SMC_RMM_REC_CREATE RMI call invoked from 
the host should be able to read the rec parameter provided by the host located
in the NS memory. Note that the page containing the rec_params is not delegated
to the RMM, so it belongs to NS_PAS. Therefore, to securely read the params 
it should be copied from the NS to REALM_PAS memory. To this end, the mapping is 
generated and the data can be safely copied from the NS memory to RMM internal 
memory buffer, rec_params. 

To be able to access REC memory, host should have invoked the delegate RMI call 
to delegate the page containing the REC. Unless, RMM rejects SMC_RMM_REC_CREATE
call because it has not been delegated, which is turned out as a result of 
invoking the find_lock_two_granules function. 

Anyway, if the page has been delegated, which means the REC page is not actually
belong to the NS PAS, then it can be mapped and accessible inside the RMM layer
directly without copies. All pages delegated to the RMM have same state 
GRANULE_STATE_DELEGATED. After initializing rec structure, it converts state of 
the rec page to GRANULE_STATE_REC by invoking granule_unlock_transition. 

### Init virtualization related registers
```cpp
static void init_rec_regs(struct rec *rec,
                          struct rmi_rec_params *rec_params,
                          struct rd *rd)
{
        unsigned int i;

        /*
         * We only need to set non-zero values here because we're intializing
         * data structures in the rec granule which was just converted from
         * the DELEGATED state to REC state, and we can rely on the RMM
         * invariant that DELEGATED granules are always zero-filled.
         */

        for (i = 0U; i < REC_CREATE_NR_GPRS; i++) {
                rec->regs[i] = rec_params->gprs[i];
        }

        rec->pc = rec_params->pc;
        rec->pstate = SPSR_EL2_MODE_EL1h |
                      SPSR_EL2_nRW_AARCH64 |
                      SPSR_EL2_F_BIT |
                      SPSR_EL2_I_BIT |
                      SPSR_EL2_A_BIT |
                      SPSR_EL2_D_BIT;

        init_rec_sysregs(rec, rec_params->mpidr);
        init_common_sysregs(rec, rd);
}



static void init_common_sysregs(struct rec *rec, struct rd *rd)
{
        unsigned long mdcr_el2_val = read_mdcr_el2();

        /* Set non-zero values only */
        rec->common_sysregs.hcr_el2 = HCR_FLAGS;
        rec->common_sysregs.vtcr_el2 =  realm_vtcr(rd);
        rec->common_sysregs.vttbr_el2 = (granule_addr(rd->s2_ctx.g_rtt) &
                                        MASK(TTBRx_EL2_BADDR)) |
                                        INPLACE(VTTBR_EL2_VMID, rd->s2_ctx.vmid);

        /* Control trapping of accesses to PMU registers */
        if (rd->pmu_enabled) {
                mdcr_el2_val &= ~(MDCR_EL2_TPM_BIT | MDCR_EL2_TPMCR_BIT);
        } else {
                mdcr_el2_val |= (MDCR_EL2_TPM_BIT | MDCR_EL2_TPMCR_BIT);
        }

        rec->common_sysregs.mdcr_el2 = mdcr_el2_val;
}
```

One of the important job of creating REC is initializing registers related with
running CCA VMs including configuring the system registers such as vttbr_el2
controlling the stage 2 paging for the VM. The base address of the stage 2 page
table was provided by the host when the realm had been created. As shown in the
above code, rd->s2_ctx.g_rtt is used to assign root stage 2 page table for 
initializing the vttbr_el2 register. The initialized registers stored in the rec
will be used when the PE enters the target VM. 

### Purpose of aux pages attached to rec? (rec->g_aux)
refer to [[]]



## SMC_RMM_REC_ENTER
```cpp
/*
 * Structure contains shared information between RMM and Host
 * during REC entry and REC exit.
 */
struct rmi_rec_run {
        /* Entry information */
        SET_MEMBER_RMI(struct rmi_rec_entry entry, 0, 0x800);   /* Offset 0 */
        /* Exit information */
        SET_MEMBER_RMI(struct rmi_rec_exit exit, 0x800, 0x1000);/* 0x800 */
};      

/*      
 * Structure contains data passed from the Host to the RMM on REC entry
 */     
struct rmi_rec_entry {           
        /* Flags */
        SET_MEMBER_RMI(unsigned long flags, 0, 0x200);  /* Offset 0 */
        /* General-purpose registers */
        SET_MEMBER_RMI(unsigned long gprs[REC_EXIT_NR_GPRS], 0x200, 0x300); /* 0x200 */
        SET_MEMBER_RMI(struct {
                        /* GICv3 Hypervisor Control Register */
                        unsigned long gicv3_hcr;                        /* 0x300 */
                        /* GICv3 List Registers */
                        unsigned long gicv3_lrs[REC_GIC_NUM_LRS];       /* 0x308 */
                   }, 0x300, 0x800);
};      

/*
 * Structure contains data passed from the RMM to the Host on REC exit
 */
struct rmi_rec_exit {
        /* Exit reason */
        SET_MEMBER_RMI(unsigned long exit_reason, 0, 0x100);/* Offset 0 */
        SET_MEMBER_RMI(struct {
                        /* Exception Syndrome Register */
                        unsigned long esr;              /* 0x100 */
                        /* Fault Address Register */
                        unsigned long far;              /* 0x108 */
                        /* Hypervisor IPA Fault Address register */
                        unsigned long hpfar;            /* 0x110 */
                   }, 0x100, 0x200);
        /* General-purpose registers */
        SET_MEMBER_RMI(unsigned long gprs[REC_EXIT_NR_GPRS], 0x200, 0x300); /* 0x200 */
        SET_MEMBER_RMI(struct {
                        /* GICv3 Hypervisor Control Register */
                        unsigned long gicv3_hcr;        /* 0x300 */
                        /* GICv3 List Registers */
                        unsigned long gicv3_lrs[REC_GIC_NUM_LRS]; /* 0x308 */
                        /* GICv3 Maintenance Interrupt State Register */
                        unsigned long gicv3_misr;       /* 0x388 */
                        /* GICv3 Virtual Machine Control Register */
                        unsigned long gicv3_vmcr;       /* 0x390 */
                   }, 0x300, 0x400);
        SET_MEMBER_RMI(struct {
                        /* Counter-timer Physical Timer Control Register */
                        unsigned long cntp_ctl;         /* 0x400 */
                        /* Counter-timer Physical Timer CompareValue Register */
                        unsigned long cntp_cval;        /* 0x408 */
                        /* Counter-timer Virtual Timer Control Register */
                        unsigned long cntv_ctl;         /* 0x410 */
                        /* Counter-timer Virtual Timer CompareValue Register */
                        unsigned long cntv_cval;        /* 0x418 */
                   }, 0x400, 0x500);
        SET_MEMBER_RMI(struct {
                        /* Base address of pending RIPAS change */
                        unsigned long ripas_base;       /* 0x500 */
                        /* Size of pending RIPAS change */
                        unsigned long ripas_size;       /* 0x508 */
                        /* RIPAS value of pending RIPAS change */
                        unsigned char ripas_value;      /* 0x510 */
                   }, 0x500, 0x600);
        /* Host call immediate value */
        SET_MEMBER_RMI(unsigned int imm, 0x600, 0x700); /* 0x600 */

        /* PMU overflow */
        SET_MEMBER_RMI(unsigned long pmu_ovf, 0x700, 0x708);     /* 0x700 */

        /* PMU interrupt enable */
        SET_MEMBER_RMI(unsigned long pmu_intr_en, 0x708, 0x710); /* 0x708 */

        /* PMU counter enable */
        SET_MEMBER_RMI(unsigned long pmu_cntr_en, 0x710, 0x800); /* 0x710 */
};
```

Host provides the rmi_rec_run page to communicate information between the RMM
before and after running the realm. rmi_rec_entry page is mapped and copied to
the RMM to securely configure the Realm according to the provided information. 

### Handling mmio and fault injection from host
```cpp
        if (!complete_mmio_emulation(rec, &rec_run.entry)) {
                ret = RMI_ERROR_REC;
                goto out_unmap_buffers;
        }

        if (!complete_sea_insertion(rec, &rec_run.entry)) {
                ret = RMI_ERROR_REC;
                goto out_unmap_buffers;
        }
```


### Complete RIPAS 
```cpp
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
I cannot understand this code snippet. 


### Changing context from NS -> Realm
```cpp
        save_ns_state(rec);
        restore_realm_state(rec);

```





## EL3 xlat 
setup_page_tables ->  mmap_add -> mmap_add_ctx -> mmap_add_region_ctx -> xlat_tables_map_region 
-> xlat_desc -> xlat_arch_get_pas (return bits corresponding to the pas)
