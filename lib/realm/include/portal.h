#ifndef PORTAL_H
#define PORTAL_H


#define SYSTEM_REALM 0x1

//global variable for system realm's rd
extern unsigned long rd_system_realm_addr;


#if 0
struct p_cmd_queue {
};
#endif 


enum portal_event {
	CMD_Q_ATTACH_INT=0x1,	
	DEV_ATTACH_INT,
	DEV_DETACH_INT,
	PORTAL_EVENT_COUNT
} ;
#endif /* PORTAL_H */
