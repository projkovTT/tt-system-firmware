/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * FLL droop detector (clock stretcher) control.
 *
 * pll_cntl_wrapper implements a two-stage droop detector with a programmable
 * response via the PLL dividers, a spike (overshoot) detector, and
 * programmable recovery timers. None of it is configured by firmware: the
 * detector is disabled out of reset (FLL_CNTL_3[3] dd_enable == 0) and every
 * threshold and adjustment reads back as zero on a stock part.
 *
 * This file adds no initialisation. It is a pure read/write interface driven
 * by TT_SMC_MSG_FLL_DROOP so the detector can be characterised from the host:
 * when to stretch, how much to stretch, how long to stretch, and how fast to
 * recover. Nothing here is enabled by default.
 *
 * Register map (base 0x80020000), from arc.json in the syseng repo,
 * cross-checked against quantum_bootcode's arc_ss_reg.h:
 *
 *   FLL_CNTL_0 0x004  [14] two_step_droop, [13] force_droop2_detect,
 *                     [12:10] dd_clk_sel, [9] dd_postdiv_ctrl_en,
 *                     [8] dd_fbdiv_ctrl_en, [7] force_spike_detect,
 *                     [6] manual_spike, [5] force_droop_detect,
 *                     [4:0] osc_ctrl_coarse
 *   FLL_CNTL_1 0x008  [31:16] droop_threshold, [15:0] droop2_threshold
 *   FLL_CNTL_2 0x00c  [31:16] droop_time, [15:8] dd_filt_sample_period,
 *                     [7:0] dd_counter_period
 *   FLL_CNTL_3 0x010  [31:16] droop_fbdiv_adj, [15:8] droop_postdiv_adj,
 *                     [7] roclk_bypass0, [6] dbuf_en, [5] roclk_bypass,
 *                     [4] dd_use_fw_nominal_vdd, [3] dd_enable,
 *                     [2] osc_enable, [1] dd_use_nominal_vdd,
 *                     [0] dd_force_sample_nominal_vdd
 *   FLL_CNTL_4 0x014  [31:16] fbdiv_recover_timer, [15:0] post_recover_timer
 *   FLL_CNTL_5 0x018  [31:16] spike_threshold, [15:0] dd_fw_nominal_vdd
 *   FLL_STATUS_0 0x01c RO counter debug readout, read-only
 *   (0x020)           not in the a0 register map
 *   FLL_CNTL_8 0x024  [31:16] droop2_time, [15:0] droop2_fbdiv_adj
 *   FLL_CNTL_9 0x028  [20] ones_on_empty_mode, [19] sample_delta_mode,
 *                     [18] manual_droop, [17:16] debug_data_mode,
 *                     [15:0] droop2_postdiv_adj
 *
 * Register indices here follow the offset: index n is at 0x80020004 + 4 * n,
 * so index 6 is 0x01c and index 7 is 0x020.
 *
 * 0x01c is FLL_STATUS_0, not an FLL_CNTL register. With FLL_CNTL_9's
 * debug_data_mode == 0 each read returns two 16-bit RO counter samples, and
 * reads consume samples, so it is read-only here and deliberately left out of
 * DUMP. Use FLL_DROOP_OP_RO_COUNT to read it.
 *
 * Whether anything exists at 0x020 is UNKNOWN: it is not in the a0 map. This
 * interface refuses to touch it rather than risk a bus error on an unmapped
 * address, and returns -ENOTSUP to distinguish it from an out-of-range index.
 *
 * A trap worth knowing before enabling anything here: the detector compares
 * each RO snapshot against a held nominal count, and that nominal has to track
 * VCORE. Either let the hardware latch it (FLL_CNTL_3 dd_use_nominal_vdd, with
 * FLL_DROOP_OP_SAMPLE_NOMINAL to re-latch after a voltage change) or supply it
 * from firmware (dd_use_fw_nominal_vdd plus FLL_CNTL_5 dd_fw_nominal_vdd).
 * Setting dd_enable with a stale nominal will either stretch permanently or
 * never stretch at all.
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <tenstorrent/smc_msg.h>
#include <tenstorrent/msgqueue.h>

#include "timer.h"
#include "reg.h"

#define PLL_CNTL_WRAPPER_FLL_CNTL_0_REG_ADDR 0x80020004
#define FLL_CNTL_REG_ADDR(i)                 (PLL_CNTL_WRAPPER_FLL_CNTL_0_REG_ADDR + 4U * (i))

/* Highest index named by the register map. */
#define FLL_INDEX_MAX 9

/* Index 6 (offset 0x01c) is FLL_STATUS_0: readable, never writable. */
#define FLL_STATUS_0_INDEX    6
#define FLL_STATUS_0_REG_ADDR FLL_CNTL_REG_ADDR(FLL_STATUS_0_INDEX)

/* Bitmap of the indices that are readable at all. 7 (0x020) is not one. */
#define FLL_READABLE_MASK                                                                          \
	(BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(4) | BIT(5) | BIT(FLL_STATUS_0_INDEX) |           \
	 BIT(8) | BIT(9))

/* The writable FLL_CNTL registers, in order. Also the DUMP window. */
static const uint8_t fll_cntl_implemented[] = {0, 1, 2, 3, 4, 5, 8, 9};

/* Response words available to a dump: data[0] carries the return code. */
#define FLL_DUMP_MAX_WORDS (RESPONSE_MSG_LEN - 1)

/* Trigger bit positions. */
#define FLL_CNTL_0_FORCE_DROOP_DETECT  BIT(5)
#define FLL_CNTL_0_MANUAL_SPIKE        BIT(6)
#define FLL_CNTL_0_FORCE_SPIKE_DETECT  BIT(7)
#define FLL_CNTL_0_FORCE_DROOP2_DETECT BIT(13)
#define FLL_CNTL_9_MANUAL_DROOP        BIT(18)

#define FLL_TRIGGER_ALL                                                                            \
	(FLL_DROOP_TRIGGER_DROOP | FLL_DROOP_TRIGGER_DROOP2 | FLL_DROOP_TRIGGER_SPIKE |            \
	 FLL_DROOP_TRIGGER_MANUAL_SPIKE | FLL_DROOP_TRIGGER_MANUAL_DROOP)

/* Default and maximum hold time for a forced trigger, in REFCLK cycles.
 * Capped so a host request cannot stall the message queue for long.
 */
#define FLL_PULSE_DEFAULT_CYCLES 10U
#define FLL_PULSE_MAX_CYCLES     (WAIT_1MS)

/* FLL_CNTL_3[0] dd_force_sample_nominal_vdd. */
#define FLL_CNTL_3_INDEX                      3
#define FLL_CNTL_3_FORCE_SAMPLE_NOMINAL_VDD   BIT(0)

/* FLL_CNTL_9[17:16] debug_data_mode. Only mode 0 packs two 16-bit samples. */
#define FLL_CNTL_9_INDEX            9
#define FLL_CNTL_9_DEBUG_DATA_MODE  GENMASK(17, 16)

/* FLL_STATUS_0 sampling defaults and limits. */
#define FLL_RO_DEFAULT_READS   32U
#define FLL_RO_DEFAULT_DISCARD 10U
#define FLL_RO_MAX_READS       64U
#define FLL_RO_MAX_DISCARD     64U

static int fll_check_read_index(uint32_t index)
{
	if (index > FLL_INDEX_MAX) {
		return -EINVAL;
	}

	if ((FLL_READABLE_MASK & BIT(index)) == 0) {
		/* Offset 0x020: not in the a0 map, existence unknown. */
		return -ENOTSUP;
	}

	return 0;
}

static int fll_check_write_index(uint32_t index)
{
	int err = fll_check_read_index(index);

	if (err != 0) {
		return err;
	}

	if (index == FLL_STATUS_0_INDEX) {
		/* FLL_STATUS_0 is a status readout, not a control register. */
		return -EACCES;
	}

	return 0;
}

static uint8_t fll_droop_get(const struct fll_droop_rqst *rqst, struct response *response)
{
	int err = fll_check_read_index(rqst->arg0);

	if (err != 0) {
		return err;
	}

	response->data[1] = ReadReg(FLL_CNTL_REG_ADDR(rqst->arg0));
	response->data[2] = FLL_CNTL_REG_ADDR(rqst->arg0);

	return 0;
}

static uint8_t fll_droop_dump(const struct fll_droop_rqst *rqst, struct response *response)
{
	uint32_t start = rqst->arg0;

	if (start >= ARRAY_SIZE(fll_cntl_implemented)) {
		return -EINVAL;
	}

	for (uint32_t i = 0; i < FLL_DUMP_MAX_WORDS; i++) {
		uint32_t entry = start + i;

		if (entry >= ARRAY_SIZE(fll_cntl_implemented)) {
			break;
		}

		response->data[1 + i] = ReadReg(FLL_CNTL_REG_ADDR(fll_cntl_implemented[entry]));
	}

	return 0;
}

static uint8_t fll_droop_set(const struct fll_droop_rqst *rqst, struct response *response)
{
	int err = fll_check_write_index(rqst->arg0);
	uint32_t addr;
	uint32_t mask;
	uint32_t old_val;
	uint32_t new_val;

	if (err != 0) {
		return err;
	}

	addr = FLL_CNTL_REG_ADDR(rqst->arg0);
	/* A zero mask means "write the whole register", which is the natural
	 * reading of a request that does not bother with a mask at all.
	 */
	mask = (rqst->arg2 == 0) ? UINT32_MAX : rqst->arg2;

	old_val = ReadReg(addr);
	new_val = (old_val & ~mask) | (rqst->arg1 & mask);
	WriteReg(addr, new_val);

	response->data[1] = ReadReg(addr);
	response->data[2] = old_val;

	return 0;
}

static uint8_t fll_droop_pulse(const struct fll_droop_rqst *rqst, struct response *response)
{
	uint32_t triggers = rqst->arg0;
	uint32_t cycles = rqst->arg1;
	uint32_t cntl0;
	uint32_t cntl9;
	uint32_t set0 = 0;
	uint32_t set9 = 0;

	if (triggers == 0 || (triggers & ~(uint32_t)FLL_TRIGGER_ALL) != 0) {
		return -EINVAL;
	}

	if (cycles == 0) {
		cycles = FLL_PULSE_DEFAULT_CYCLES;
	} else if (cycles > FLL_PULSE_MAX_CYCLES) {
		cycles = FLL_PULSE_MAX_CYCLES;
	}

	if (triggers & FLL_DROOP_TRIGGER_DROOP) {
		set0 |= FLL_CNTL_0_FORCE_DROOP_DETECT;
	}
	if (triggers & FLL_DROOP_TRIGGER_DROOP2) {
		set0 |= FLL_CNTL_0_FORCE_DROOP2_DETECT;
	}
	if (triggers & FLL_DROOP_TRIGGER_SPIKE) {
		set0 |= FLL_CNTL_0_FORCE_SPIKE_DETECT;
	}
	if (triggers & FLL_DROOP_TRIGGER_MANUAL_SPIKE) {
		set0 |= FLL_CNTL_0_MANUAL_SPIKE;
	}
	if (triggers & FLL_DROOP_TRIGGER_MANUAL_DROOP) {
		set9 |= FLL_CNTL_9_MANUAL_DROOP;
	}

	cntl0 = ReadReg(FLL_CNTL_REG_ADDR(0));
	cntl9 = ReadReg(FLL_CNTL_REG_ADDR(9));

	if (set0 != 0) {
		WriteReg(FLL_CNTL_REG_ADDR(0), cntl0 | set0);
	}
	if (set9 != 0) {
		WriteReg(FLL_CNTL_REG_ADDR(9), cntl9 | set9);
	}

	Wait(cycles);

	/* Deassert only the bits we asserted; leave the rest as we found them. */
	if (set0 != 0) {
		WriteReg(FLL_CNTL_REG_ADDR(0), cntl0 & ~set0);
	}
	if (set9 != 0) {
		WriteReg(FLL_CNTL_REG_ADDR(9), cntl9 & ~set9);
	}

	response->data[1] = ReadReg(FLL_CNTL_REG_ADDR(0));
	response->data[2] = ReadReg(FLL_CNTL_REG_ADDR(9));

	return 0;
}

/*
 * Average a burst of RO counter samples out of FLL_STATUS_0.
 *
 * Each read returns two 16-bit samples packed into one word, but only when
 * FLL_CNTL_9's debug_data_mode is 0. Any other mode packs 8/4/1-bit values and
 * the average below would be meaningless, so refuse rather than return a
 * plausible-looking wrong number.
 *
 * Reads consume samples, so the first few are discarded to flush whatever was
 * already in the debug buffer.
 */
static uint8_t fll_droop_ro_count(const struct fll_droop_rqst *rqst, struct response *response)
{
	uint32_t reads = (rqst->arg0 == 0) ? FLL_RO_DEFAULT_READS : rqst->arg0;
	uint32_t discard = (rqst->arg1 == 0) ? FLL_RO_DEFAULT_DISCARD : rqst->arg1;
	uint32_t cntl9 = ReadReg(FLL_CNTL_REG_ADDR(FLL_CNTL_9_INDEX));
	uint32_t accum = 0;
	uint32_t min = UINT16_MAX;
	uint32_t max = 0;
	uint32_t samples;

	if ((cntl9 & FLL_CNTL_9_DEBUG_DATA_MODE) != 0) {
		/* Not the 16-bit packing this unpack assumes. */
		return -ENOTSUP;
	}

	reads = MIN(reads, FLL_RO_MAX_READS);
	discard = MIN(discard, FLL_RO_MAX_DISCARD);

	for (uint32_t i = 0; i < discard; i++) {
		(void)ReadReg(FLL_STATUS_0_REG_ADDR);
	}

	for (uint32_t i = 0; i < reads; i++) {
		uint32_t word = ReadReg(FLL_STATUS_0_REG_ADDR);
		uint32_t lo = word & 0xFFFFU;
		uint32_t hi = (word >> 16) & 0xFFFFU;

		accum += lo + hi;
		min = MIN(min, MIN(lo, hi));
		max = MAX(max, MAX(lo, hi));
	}

	samples = reads * 2U;

	response->data[1] = accum / samples;
	response->data[2] = min;
	response->data[3] = max;
	response->data[4] = samples;

	return 0;
}

/*
 * Re-latch the hardware-sampled nominal RO count at the current operating
 * point by pulsing FLL_CNTL_3 dd_force_sample_nominal_vdd.
 *
 * Only meaningful while dd_use_nominal_vdd is selected; with
 * dd_use_fw_nominal_vdd the nominal comes from FLL_CNTL_5 instead.
 */
static uint8_t fll_droop_sample_nominal(struct response *response)
{
	uint32_t addr = FLL_CNTL_REG_ADDR(FLL_CNTL_3_INDEX);
	uint32_t cntl3 = ReadReg(addr);

	WriteReg(addr, cntl3 | FLL_CNTL_3_FORCE_SAMPLE_NOMINAL_VDD);
	WriteReg(addr, cntl3 & ~(uint32_t)FLL_CNTL_3_FORCE_SAMPLE_NOMINAL_VDD);

	response->data[1] = ReadReg(addr);

	return 0;
}

/**
 * @brief Handler for TT_SMC_MSG_FLL_DROOP messages
 *
 * @details Reads back, programs, or force-triggers the FLL droop detector
 *          (clock stretcher) in pll_cntl_wrapper. The detector is disabled out
 *          of reset and this handler is the only thing in firmware that
 *          touches it, so nothing changes until the host asks for it.
 *
 * @param request Pointer to the host request message, use request->fll_droop for structured access
 * @param response Pointer to the response message to be sent back to host
 *
 * @return 0 on success
 * @return (uint8_t)-EINVAL on a bad operation, index or trigger mask
 * @return (uint8_t)-EACCES on a write to FLL_STATUS_0, which is read-only
 * @return (uint8_t)-ENOTSUP for index 7, which the a0 register map does not
 *         define, or for RO_COUNT while debug_data_mode is not 0
 *
 * @see fll_droop_rqst
 */
static uint8_t fll_droop_handler(const union request *request, struct response *response)
{
	const struct fll_droop_rqst *rqst = &request->fll_droop;

	switch (rqst->op) {
	case FLL_DROOP_OP_GET:
		return fll_droop_get(rqst, response);
	case FLL_DROOP_OP_DUMP:
		return fll_droop_dump(rqst, response);
	case FLL_DROOP_OP_SET:
		return fll_droop_set(rqst, response);
	case FLL_DROOP_OP_PULSE:
		return fll_droop_pulse(rqst, response);
	case FLL_DROOP_OP_RO_COUNT:
		return fll_droop_ro_count(rqst, response);
	case FLL_DROOP_OP_SAMPLE_NOMINAL:
		return fll_droop_sample_nominal(response);
	default:
		return -EINVAL;
	}
}
REGISTER_MESSAGE(TT_SMC_MSG_FLL_DROOP, fll_droop_handler);
