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


unsigned long handle_rsi_device_manage(struct rec *rec, struct rmi_rec_exit *rec_exit)
{
	//lookup device lists 
	rb_node *dev_node;
	device_info *dev_info;

	unsigned long base_addr = rec->regs[1];
	unsigned long cmd = rec->regs[2];
	INFO("%s: device base addr:%lx cmd:%lx\n",
			__func__, base_addr, cmd);
	

	dev_node = search_rb_tree(&rb_dev_tree, base_addr);
	if (!dev_node) {
		dev_info = &(dev_node->dev_info);
		INFO("Device %s needs to be attached\n", dev_info->dev_name);
	} else {
		//no matching device 
		;
	}
	
	return 0;

}

unsigned long handle_rsi_attach_device(struct rec *rec, struct rmi_rec_exit *rec_exit)
{
	//lookup device lists 
	rb_node *dev_node;
	device_info *dev_info;

	unsigned long base_addr = rec->regs[1];
	unsigned long cmd = rec->regs[2];
	INFO("%s: device base addr:%lx cmd:%lx\n",
			__func__, base_addr, cmd);

	dev_node = search_rb_tree(&rb_dev_tree, base_addr);
	if (!dev_node) {
		dev_info = &(dev_node->dev_info);
		INFO("Device %s needs to be attached\n", dev_info->dev_name);
	} else {
		//no matching device 
		;
	}
	return 0;
}
