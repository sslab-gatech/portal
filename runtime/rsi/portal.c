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


unsigned long handle_rsi_portal_dev_mng(struct rec *rec, struct rmi_rec_exit *rec_exit)
{
	//lookup device lists 
	rb_node *dev_node = NULL;
	device_info *dev_info;

	unsigned long base_addr = rec->regs[1];
	unsigned long cmd = rec->regs[2];
	unsigned long requester_rd = granule_addr(rec->realm_info.g_rd);
	unsigned long requestee_rd = 0UL;
	unsigned long host_cmd = 0UL;

	/* for device testing temporarily generate device info */
	static int init = 0;
	if (init == 0) {
		init = 1;
		device_info test_dev = {.base=0xdeadbeef, .size =0x1000, .state=DEV_EMPTY,
					.prev_state=DEV_EMPTY, .owner_rd_addr = requester_rd,
					.dev_name="PortalTestingDev"}; 
		rb_insert(&rb_dev_tree, test_dev);
		rd_system_realm_addr = requester_rd; 
	}
	/* end of test code */

	INFO("[RMM:%s]Requester RD:%lx\n", __func__,requester_rd);
	
	dev_node = search_rb_tree(&rb_dev_tree, base_addr);
	if (dev_node) {
		dev_info = &(dev_node->dev_info);
		INFO("[RMM:%s]: device found on the rmm dev list! base addr:%lx, state:%x cmd:%lx\n",
				__func__, dev_info->base, dev_info->state, cmd);
		switch ((enum portal_dev_mng_cmd)cmd) {
			case DEV_ATTACH:
				/* Only system realm can occupy SMMU device */
				if (is_smmu(base_addr) && !is_system_realm(requester_rd)) {
					ERROR("Only system Realm can occupy SMMU device");
					return RMI_ERROR_REALM;
				}               
				switch (dev_info->state) {
					/* Request device to host */
					case DEV_EMPTY:
						dev_info->state = DEV_IN_TRANSIT;
						dev_info->prev_state = DEV_EMPTY;
						requestee_rd = rd_system_realm_addr;
						host_cmd = DEV_DELEGATE;
						/* Exit to host kernel to ask device delegation */
						goto exit_to_host;
					/* Currently just grab the device from other realm.
					 * In the future it needs internal scheduler to 
					 * decide whether the device should be attached
					 */
					case DEV_OCCUPIED:
						dev_info->state = DEV_IN_TRANSIT;
						dev_info->prev_state = DEV_OCCUPIED;
						host_cmd = DEV_INVOKE_REALM;
						/* Switch owner */
						requestee_rd = dev_info->owner_rd_addr;
						dev_info->owner_rd_addr = requester_rd; 
						break;
					/* cannot occupy the device in-transit */
					case DEV_IN_TRANSIT:
					case DEV_DETACHED:
						return RMI_ERROR_IN_USE;

					default:
						return RMI_ERROR_INPUT;
				}
				break;

			case DEV_DETACH:
				switch (dev_info->state) {
					case DEV_IN_TRANSIT:
						if (dev_info->prev_state == DEV_OCCUPIED) {
							/* Destroy device mapping in s2tt */

							/* Exit to system REALM */
							dev_info->state = DEV_DETACHED;
							requestee_rd = rd_system_realm_addr;
							host_cmd = DEV_INVOKE_REALM;
							break;
						} //else -> fall through to default case
					/* all other cases are error */
					default:
						return RMI_ERROR_INPUT;
						break;
				}
				break;
			case DEV_SMMU_MAPPED:
				/* Occupy cmd can be only be issued from system realm.
				 * After the SMMU setting is done, it can finally be 
				 * assigned to the realm 
				 */
				if (!is_system_realm(requester_rd))
					return RMI_ERROR_REALM;
				switch (dev_info->state) {
					case DEV_DETACHED:
						/* Establish device mapping in s2tt  */

						dev_info->state = DEV_OCCUPIED;
						break;
					case DEV_IN_TRANSIT:
						if (dev_info->prev_state == DEV_EMPTY) {
							/* Device is delegated from NW  */
							dev_info->state = DEV_OCCUPIED;
							break;
						} else {
							return RMI_ERROR_INPUT;
						}
					default:
						return RMI_ERROR_INPUT;
				}
				break;

			default:
				return RMI_ERROR_INPUT;

		}
	} else {
		//no matching device 
		INFO("[RMM:%s]:No matching device", __func__);
		return RMI_ERROR_INPUT;
	}
	
exit_to_host:
	//needs to exit host to invoke system Realm 
	INFO("[RMM]:Exiting to host!\n");
	INFO("[RMM]:Base: %lx, size: %lx, target_rd: %lx, flag:%lx\n",
		dev_info->base, dev_info->size, requestee_rd, host_cmd);
	rec_exit->portal_dev_base = dev_info->base;
	rec_exit->portal_dev_size = dev_info->size;
	rec_exit->portal_dev_target_rd = requestee_rd;
	rec_exit->portal_dev_flag = host_cmd;
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
