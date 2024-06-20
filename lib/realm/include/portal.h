#ifndef PORTAL_H
#define PORTAL_H

//XXX{Need to be moved to compiler option..}
#define ENABLE_PORTAL 0x1
#define SYSTEM_REALM 0x1


//global variable for system realm's rd
extern unsigned long rd_system_realm_addr;


#if 0
struct p_cmd_queue {
};
#endif 

struct portal_dev {
;
};

enum portal_event {
	CMD_Q_ATTACH_INT=0x1,	
	DEV_ATTACH_INT,
	DEV_DETACH_INT,
	PORTAL_EVENT_COUNT
} ;
#endif /* PORTAL_H */
