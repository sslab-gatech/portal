#include <buffer.h>
#include <granule.h>
#include <realm.h>
#include <portal.h>
#include <smc-handler.h>
#include <smc-rmi.h>
#include <smc.h>
#include <stddef.h>
#include <string.h>
#include <debug.h>


unsigned long smc_portal_dev_manage(unsigned long rd_addr,
				    unsigned long dev_addr, 
				    unsigned long cmd)
{
	/* search device rb_tree. The base address should exactly match */
	rb_node *dev_node = search_rb_tree(&rb_dev_tree, dev_addr);
	if (dev_node == NULL) {
		INFO("Requested device does not exist \n");
		return RMI_ERROR_INPUT;
	} 

	switch ((enum portal_dev_mng_cmd)cmd) {
		case DEV_ATTACH: 

		default:
			return RMI_ERROR_INPUT;
	}




	return 0;
}

unsigned long smc_portal_create_queue(unsigned long q_addr,
	       			  unsigned long rd_addr,
				  unsigned long system_rd_addr)
{

	struct granule *g_rd, *g_system_rd, *g_queue;
	struct granule *g_table_root;
	struct rd *rd, *system_rd;
	struct p_cmd_queue *cmd_queue;
	enum ripas ripas;
	unsigned long s2tte, *s2tt; 
	struct rtt_walk wi;
	int sl;
	unsigned long ipa_bits;
	unsigned long ret = RMI_SUCCESS;
	//unsigned int vmid; 
	//vmid = rd->s2_ctx.vmid;
	
	//check the provided address is system realm's rd
	assert(!is_system_realm(system_rd_addr));
	if (!find_lock_two_granules(q_addr, 
				    GRANULE_STATE_DELEGATED,
				    &g_queue,
				    rd_addr, 
				    GRANULE_STATE_RD,
				    &g_rd)) {
		return RMI_ERROR_INPUT;
	}
	rd = granule_map(g_rd, SLOT_RD);
	cmd_queue = granule_map(g_queue, SLOT_DELEGATED);

	//initialize command queue 

	//bind command queue to the target RD
	rd->cmd_queue = cmd_queue;
	granule_unlock(g_rd);

	//generate s2tte mapping to queue for system realm
	g_system_rd = find_lock_granule(system_rd_addr, GRANULE_STATE_RD);
	if (g_system_rd == NULL) {
		return RMI_ERROR_INPUT;
	}
	system_rd = granule_map(g_system_rd, SLOT_RD);

	g_table_root = system_rd->s2_ctx.g_rtt;
	sl = realm_rtt_starting_level(system_rd);
	ipa_bits = realm_ipa_bits(system_rd);
	granule_lock(g_table_root, GRANULE_STATE_RTT);
	rtt_walk_lock_unlock(g_table_root, sl, ipa_bits,
			     q_addr, RTT_PAGE_LEVEL, &wi);
			
	if (wi.last_level != RTT_PAGE_LEVEL) {
		ret = pack_return_code(RMI_ERROR_RTT, wi.last_level);
		goto out_unlock_ll_table; 
	}
	
	s2tt = granule_map(wi.g_llt, SLOT_RTT);
	s2tte = s2tte_read(&s2tt[wi.index]);
	if (!s2tte_is_unassigned(s2tte)) {
		ret = pack_return_code(RMI_ERROR_RTT, RTT_PAGE_LEVEL);
		goto out_unmap_ll_table; 
	}
	
	ripas = s2tte_get_ripas(s2tte);
	s2tte = (ripas == RIPAS_EMPTY)? 
		s2tte_create_assigned_empty(q_addr, RTT_PAGE_LEVEL) :
		s2tte_create_valid(q_addr, RTT_PAGE_LEVEL);

	s2tte_write(&s2tt[wi.index], s2tte);
	s2tte = s2tte_read(&s2tt[wi.index]);
	
	/* set flag in system realm's rd so that interrupt can be handled
	 * by the next REC_ENTER. */
	system_rd->portal_event = CMD_Q_ATTACH_INT;
	
	__granule_get(wi.g_llt);

	
out_unmap_ll_table: 
	buffer_unmap(s2tt);
out_unlock_ll_table:
	granule_unlock(wi.g_llt);

	buffer_unmap(rd);
	buffer_unmap(system_rd);
	granule_unlock(g_system_rd);
	granule_unlock_transition(g_queue, GRANULE_STATE_CMD_QUEUE);

	return ret;
}
