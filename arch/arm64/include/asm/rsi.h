/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2023 ARM Ltd.
 */

#ifndef __ASM_RSI_H_
#define __ASM_RSI_H_

#include <linux/jump_label.h>
#include <asm/rsi_cmds.h>

extern struct static_key_false rsi_present;

void arm64_setup_memory(void);

void __init arm64_rsi_init(void);
static inline bool is_realm_world(void)
{
	return static_branch_unlikely(&rsi_present);
}

static inline void set_memory_range(phys_addr_t start, phys_addr_t end,
				    enum ripas state)
{
	unsigned long ret;
	phys_addr_t top;

	printk("Guest RSI!%s : %llx-%llx \n", __func__, start, end);
	while (start != end) {
		ret = rsi_set_addr_range_state(start, end, state, &top);
		BUG_ON(ret);
		BUG_ON(top < start);
		BUG_ON(top > end);
		start = top;
	}
	printk("Guest RSI!%s done\n", __func__);
}

static inline void set_memory_range_protected(phys_addr_t start, phys_addr_t end)
{
	set_memory_range(start, end, RSI_RIPAS_RAM);
}

static inline void set_memory_range_shared(phys_addr_t start, phys_addr_t end)
{
	set_memory_range(start, end, RSI_RIPAS_EMPTY);
}

static inline void set_memory_range_portal(phys_addr_t start, phys_addr_t end)
{
	set_memory_range(start, end, RSI_RIPAS_PORTAL);
}

static inline void set_memory_range_portal_executable(phys_addr_t start, phys_addr_t end)
{
	set_memory_range(start, end, RSI_RIPAS_PORTAL_EXECUTABLE);
}

static void portal_attach_dev(phys_addr_t dev_addr)
{
	pr_info("[REALM:%s]: device addr:%lx \n", __func__, dev_addr);
	rsi_device_management(dev_addr, DEV_ATTACH);
}

static void portal_detach_dev(phys_addr_t dev_addr)
{
	pr_info("%s invoked\n", __func__);
	rsi_device_management(dev_addr, DEV_DETACH);
}
#endif
