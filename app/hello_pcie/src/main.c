/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/sys/printk.h>

#include <soc.h>

int main(void)
{
	const struct device *pcie_dev = DEVICE_DT_GET(DT_NODELABEL(pcie0));

	printk("hello_pcie on %s\n", CONFIG_BOARD_TARGET);

	if (!device_is_ready(pcie_dev)) {
		printk("pcie0 not ready\n");
		return 1;
	}

	printk("pcie0 ready\n");
	test_pass();

	/* Keep the endpoint alive; do not sleep (emul mtime is ~10x slow). */
	while (1) {
	}

	return 0;
}
