## Cold boot
rmm_entry -> plat_setup -> plat_cmn_setup -> xlat_ctx_init ->  xlat_init_tables_ctx
          -> xlat_enable_mmu_el2
	  -> rmm_main
	  -> smc_ret


## Warm boot
rmm_entry -> plat_warmboot_setup ->
          -> xlat_enable_mmu_el2 ->
	  -> rmm_warmboot_main ->
          -> smc_ret



plat_cmn_warmboot_setup -> xlat_arch_setup_mmu_cfg 
                        -> slot_buf_setup_xlat
