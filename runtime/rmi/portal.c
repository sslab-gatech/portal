#include <buffer.h>
#include <granule.h>
#include <realm.h>
#include <smc-handler.h>
#include <smc-rmi.h>
#include <smc.h>
#include <stddef.h>
#include <string.h>
#include <debug.h>

unsigned long smc_portal_attach_dev(unsigned long dev_addr)
{
	return 0;
}

unsigned long smc_portal_create_q(unsigned long q_addr,
	       			  unsigned long vmid)
{
	//check it is system realm 
	
	//check granule 


	//map page in RMM


	//inject interrupt to system realm

	
	return 0;
}
