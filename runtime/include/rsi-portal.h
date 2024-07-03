#ifndef RSI_PORTAL_H
#define RSI_PORTAL_H
#include <realm.h>
#include <portal.h>

unsigned long handle_rsi_set_portal(struct rec *rec);
unsigned long handle_rsi_portal_dev_mng(struct rec *rec, struct rmi_rec_exit *rec_exit);
unsigned long handle_rsi_attach_device(struct rec *rec, struct rmi_rec_exit *rec_exit);
#endif 
