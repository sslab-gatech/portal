// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012, NVIDIA CORPORATION.  All rights reserved.
 */

#include <linux/acpi.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/clocksource.h>
#include <asm/rsi.h>

extern struct of_device_id __timer_of_table[];

static const struct of_device_id __timer_of_table_sentinel
	__used __section("__timer_of_table_end");

void __init timer_probe(void)
{
	struct device_node *np;
	const struct of_device_id *match;
	of_init_fn_1_ret init_func_ret;
	unsigned timers = 0;
	int ret;

	rsi_host_debug(5000);
	for_each_matching_node_and_match(np, __timer_of_table, &match) {
		if (!of_device_is_available(np))
			continue;

		init_func_ret = match->data;

		rsi_host_debug(5001);
		ret = init_func_ret(np);
		rsi_host_debug(5002);
		if (ret) {
			if (ret != -EPROBE_DEFER) 
				pr_err("Failed to initialize '%pOF': %d\n", np,
				       ret);
			continue;
		}

		timers++;
	}

	//timers += acpi_probe_device_table(timer); //hack to bypass acpi probing
	rsi_host_debug(timers);

	if (!timers) {
		rsi_host_debug(0xeeeeeeee);
		pr_crit("%s: no matching timers found\n", __func__);
	}
}
