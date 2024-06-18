#include <realm.h>
#include <smc.h>
#include <smc-rsi.h>
#include <smc.h>
#include <rmm_el3_ifc.h>
#include <string.h>
#include <utils_def.h>
#include <debug.h>

unsigned long handle_rsi_set_portal(struct rec *rec)
{
	//invoke smc for setting portal
	unsigned long smc_ret;
	unsigned long addr = rec->regs[1];

	INFO("%s: Portal Addr:%lx\n", __func__, addr);
	smc_ret = monitor_call(SMC_RMM_SET_PORTAL, addr, 
			 0UL, 0UL, 0UL, 0UL, 0UL);

	return smc_ret;
}
