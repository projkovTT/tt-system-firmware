/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/mbox.h>

#include <tenstorrent/msgqueue.h>
#include <tenstorrent/smc_msg.h>

#include <soc.h>

#define TEST_VALUE 0xA5A5u

int main(void)
{
	const struct device *mbox_dev = DEVICE_DT_GET(DT_NODELABEL(mbox0));
	union request req = {0};
	struct response rsp = {0};
	uint64_t doorbell = 1;
	struct mbox_msg msg = {
		.data = &doorbell,
		.size = sizeof(doorbell),
	};
	int ret;

	printk("hello_msgqueue on %s\n", CONFIG_BOARD_TARGET);

	if (!device_is_ready(mbox_dev)) {
		printk("mbox0 not ready\n");
		return 1;
	}

	init_msgqueue();

	req.test.command_code = TT_SMC_MSG_TEST;
	req.test.test_value = TEST_VALUE;
	ret = msgqueue_request_push(0, &req);
	if (ret != 0) {
		printk("msgqueue_request_push failed: %d\n", ret);
		return 1;
	}

	/* Simulate peer: write outbound mbox → inbound IRQ doorbell. */
	ret = mbox_send(mbox_dev, 0, &msg);
	if (ret != 0) {
		printk("mbox_send failed: %d\n", ret);
		return 1;
	}

	/* Yield so the mbox ISR can submit work and process the queue. */
	k_msleep(50);

	ret = msgqueue_response_pop(0, &rsp);
	if (ret != 0) {
		printk("msgqueue_response_pop failed: %d\n", ret);
		return 1;
	}

	printk("test response: data[1]=0x%x (expect 0x%x) serial=%u\n", rsp.data[1], TEST_VALUE + 1,
	       rsp.data[2]);
	printk("hello_msgqueue done\n");

	test_pass();
	return 0;
}
