#ifndef PORTAL_H
#define PORTAL_H


//XXX{Need to be moved to compiler option..}
#define SYSTEM_REALM 0x1


//global variable for system realm's rd
extern unsigned long rd_system_realm_addr;


#if 0
struct p_cmd_queue {
};

struct portal_dev {
;
};
#endif 

enum portal_event {
	CMD_Q_ATTACH_INT=0x1,	
	DEV_ATTACH_INT,
	DEV_DETACH_INT,
	PORTAL_EVENT_COUNT
};

enum portal_dev_state {
	DEV_EMPTY,
	DEV_OCCUPIED,
	DEV_IN_TRANSIT
};

enum portal_dev_mng_cmd {
	DEV_ATTACH,
	DEV_DETACH,
	DEV_OCCUPY
};
#endif /* PORTAL_H */
