#ifndef RSI_PORTAL_H
#define RSI_PORTAL_H
#include <realm.h>
#include <portal.h>

unsigned long handle_rsi_set_portal(struct rec *rec);
unsigned long handle_rsi_device_manage(struct rec *rec);
unsigned long handle_rsi_attach_device(struct rec *rec);
#endif 
