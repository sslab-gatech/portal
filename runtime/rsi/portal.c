#include <realm.h>
#include <smc.h>
#include <smc-rsi.h>
#include <smc.h>
#include <rmm_el3_ifc.h>
#include <string.h>
#include <utils_def.h>
#include <debug.h>
#include <granule.h>

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
	rb_node *dev_node = NULL;
	device_info *dev_info;

	unsigned long base_addr = rec->regs[1];
	unsigned long cmd = rec->regs[2];
	unsigned long rd_addr = granule_addr(rec->realm_info.g_rd);

	INFO("%s: device base addr:%lx cmd:%lx\n",
			__func__, base_addr, cmd);
	

	dev_node = search_rb_tree(&rb_dev_tree, base_addr);
	if (dev_node) {
		dev_info = &(dev_node->dev_info);
		switch ((enum portal_dev_mng_cmd)cmd) {
			case DEV_ATTACH:
				/* Only system realm can occupy SMMU device */
				if (is_smmu(base_addr) && !is_system_realm(rd_addr)) {
					return RMI_ERROR_REALM;
				}               
				switch (dev_info->state) {
					/* Request device to host */
					case DEV_EMPTY:
						dev_info->state = DEV_OCCUPIED;
						/* Exit to host kernel to ask device delegation */
						goto exit_to_host;
					/* Currently just grab the device from other realm.
					 * In the future it needs internal scheduler to 
					 * decide whether the device should be attached
					 */
					case DEV_OCCUPIED:
						dev_info->state = DEV_IN_TRANSIT;
						/* Exit to device owner  */

						break;
					/* cannot occupy the device in-transit */
					case DEV_IN_TRANSIT:
						return RMI_ERROR_IN_USE;

					default:
						return RMI_ERROR_INPUT;
				}
				break;

			case DEV_DETACH:
				switch (dev_info->state) {
					case DEV_IN_TRANSIT:
						/* Destroy device mapping in s2tt */

						/* Exit to system REALM */
						dev_info->state = DEV_DETACHED;
						break;

					/* all other cases are error */
					default:
						return RMI_ERROR_INPUT;
						break;
				}
				break;
			case DEV_OCCUPY:
				/* Occupy cmd can be only be issued from system realm.
				 * After the SMMU setting is done, it can finally be 
				 * assigned to the realm 
				 */

				if (!is_system_realm(rd_addr))
					return RMI_ERROR_REALM;
				switch (dev_info->state) {
					case DEV_DETACHED:
						/* Establish device mapping in s2tt  */

						dev_info->state = DEV_OCCUPIED;
						break;
					default:
						return RMI_ERROR_INPUT;
				}
				break;

			default:
				return RMI_ERROR_INPUT;

		}
	} else {
		//no matching device 
		INFO("No matching device");
		return RMI_ERROR_INPUT;
	}
	
exit_to_host:
	//needs to exit host to invoke system Realm 
	rec_exit->portal_dev_base = dev_info->base;
	rec_exit->portal_dev_base = dev_info->size;

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
