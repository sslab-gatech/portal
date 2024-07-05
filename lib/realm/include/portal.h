#ifndef PORTAL_H
#define PORTAL_H


//XXX{Need to be moved to compiler option..}
#define SYSTEM_REALM 0x1
extern unsigned long rd_system_realm_addr;

inline int is_system_realm (unsigned long rd_addr)
{
	return (rd_system_realm_addr == rd_addr);
}

#if 0
struct p_cmd_queue {
};

struct portal_dev {
;
};
#endif 

enum portal_event {
	DEV_ATTACH_INT = 0x1,
	DEV_DETACH_INT,
	CMD_Q_ATTACH_INT,	/* for system realm */
	DEV_MAP_SMMU_INT, 	/* for system realm */
	DEV_UNMAP_SMMU_INT,	/* for system realm */
	PORTAL_EVENT_COUNT,
};

enum portal_dev_state {
	DEV_EMPTY = 0x1, //device has not been delegated to Realm world
	DEV_OCCUPIED, //device is currently occupied by Realm
	DEV_IN_TRANSIT, 
	DEV_DETACHED // device is detached from realm, but SMMU mapping exist 
};

enum portal_dev_mng_cmd {
	DEV_ATTACH = 0x1,
	DEV_DETACH,
	DEV_SMMU_MAPPED
};

enum portal_dev_mng_host_cmd {
	DEV_DELEGATE = 0x1,
	DEV_INVOKE_REALM
};


extern unsigned long smmu_addr;
static inline int is_smmu(unsigned long dev_addr)
{
        return (dev_addr == smmu_addr);
}



extern unsigned int portal_interrupts[PORTAL_EVENT_COUNT];
#define PORTAL_INTERRUPT 49U 


#endif /* PORTAL_H */
