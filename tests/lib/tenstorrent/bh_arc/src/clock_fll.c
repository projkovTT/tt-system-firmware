/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Tests for TT_SMC_MSG_CLOCK_WAVE and TT_SMC_MSG_FLL_DROOP.
 *
 * Both handlers are pure register sequences, so these tests model the few
 * registers involved in a shadow table and assert on the exact order of bus
 * accesses. The ordering assertions matter: the clock wave sequence is only
 * safe because both enables are deasserted before anything else is touched.
 */

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/ztest.h>
#include <zephyr/sys/util.h>

#include <tenstorrent/msgqueue.h>
#include <tenstorrent/smc_msg.h>

#include "clock_wave.h"
#include "reg_mock.h"

#define CLOCK_WAVE_CNTL_ADDR 0x80020038
#define MODE_ADDR(i)         (0x80030B00U + 4U * (i))
#define FLL_ADDR(i)          (0x80020004U + 4U * (i))

#define REFCLK_CNT_LO_ADDR 0x800300E0
#define REFCLK_CNT_HI_ADDR 0x800300E4

/* Reset defaults: zero-skew selected, every row's retimer disabled. */
#define CNTL_RESET   0x00000001U
#define MODE_RESET   0x11111111U
#define MODE_3_RESET 0x00000011U

#define TRACE_MAX 64

struct shadow_reg {
	uint32_t addr;
	uint32_t val;
};

static struct shadow_reg shadow[] = {
	{CLOCK_WAVE_CNTL_ADDR, 0}, {MODE_ADDR(0), 0}, {MODE_ADDR(1), 0}, {MODE_ADDR(2), 0},
	{MODE_ADDR(3), 0},         {FLL_ADDR(0), 0},  {FLL_ADDR(1), 0},  {FLL_ADDR(2), 0},
	{FLL_ADDR(3), 0},          {FLL_ADDR(4), 0},  {FLL_ADDR(5), 0},  {FLL_ADDR(8), 0},
	{FLL_ADDR(9), 0},
};

/* FLL_STATUS_0 (index 6) is a streaming debug readout, not a stored value:
 * every read yields two packed 16-bit RO samples. 0x00140010 unpacks to 16 and
 * 20, so a burst averages to 18.
 */
#define FLL_STATUS_0_ADDR   FLL_ADDR(6)
#define FLL_STATUS_0_SAMPLE 0x00140010U
static unsigned int status0_reads;

static struct shadow_reg wr_trace[TRACE_MAX];
static unsigned int wr_count;
static uint32_t rd_trace[TRACE_MAX];
static unsigned int rd_count;
static uint32_t refclk_counter;

static union request cf_req;
static struct response cf_rsp;

static struct shadow_reg *shadow_find(uint32_t addr)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(shadow); i++) {
		if (shadow[i].addr == addr) {
			return &shadow[i];
		}
	}

	return NULL;
}

static uint32_t shadow_get(uint32_t addr)
{
	struct shadow_reg *reg = shadow_find(addr);

	zassert_not_null(reg, "no shadow for 0x%08x", addr);
	return reg->val;
}

static uint32_t clock_fll_read_fake(uint32_t addr)
{
	struct shadow_reg *reg;

	if (addr == REFCLK_CNT_LO_ADDR) {
		return refclk_counter++;
	}
	if (addr == REFCLK_CNT_HI_ADDR) {
		return 0;
	}

	if (addr == FLL_STATUS_0_ADDR) {
		status0_reads++;
		if (rd_count < TRACE_MAX) {
			rd_trace[rd_count] = addr;
		}
		rd_count++;
		return FLL_STATUS_0_SAMPLE;
	}

	if (rd_count < TRACE_MAX) {
		rd_trace[rd_count] = addr;
	}
	rd_count++;

	reg = shadow_find(addr);
	return (reg != NULL) ? reg->val : 0xDEADBEEF;
}

static void clock_fll_write_fake(uint32_t addr, uint32_t val)
{
	struct shadow_reg *reg = shadow_find(addr);

	if (wr_count < TRACE_MAX) {
		wr_trace[wr_count].addr = addr;
		wr_trace[wr_count].val = val;
	}
	wr_count++;

	if (reg != NULL) {
		reg->val = val;
	}
}

static bool addr_was_read(uint32_t addr)
{
	for (unsigned int i = 0; i < MIN(rd_count, (unsigned int)TRACE_MAX); i++) {
		if (rd_trace[i] == addr) {
			return true;
		}
	}

	return false;
}

static bool addr_was_written(uint32_t addr)
{
	for (unsigned int i = 0; i < MIN(wr_count, (unsigned int)TRACE_MAX); i++) {
		if (wr_trace[i].addr == addr) {
			return true;
		}
	}

	return false;
}

static void push_msg(void)
{
	memset(&cf_rsp, 0, sizeof(cf_rsp));
	msgqueue_request_push(0, &cf_req);
	process_message_queues();
	msgqueue_response_pop(0, &cf_rsp);
}

static void clear_traces(void)
{
	wr_count = 0;
	rd_count = 0;
}

static void clock_fll_before(void *fixture)
{
	(void)fixture;

	ReadReg_fake.custom_fake = clock_fll_read_fake;
	WriteReg_fake.custom_fake = clock_fll_write_fake;

	shadow_find(CLOCK_WAVE_CNTL_ADDR)->val = CNTL_RESET;
	shadow_find(MODE_ADDR(0))->val = MODE_RESET;
	shadow_find(MODE_ADDR(1))->val = MODE_RESET;
	shadow_find(MODE_ADDR(2))->val = MODE_RESET;
	shadow_find(MODE_ADDR(3))->val = MODE_3_RESET;
	for (unsigned int i = 5; i < ARRAY_SIZE(shadow); i++) {
		shadow[i].val = 0;
	}

	refclk_counter = 0;
	status0_reads = 0;
	clear_traces();
	memset(&cf_req, 0, sizeof(cf_req));
	memset(&cf_rsp, 0, sizeof(cf_rsp));
}

/* ------------------------------------------------------------------ */
/* TT_SMC_MSG_CLOCK_WAVE                                               */
/* ------------------------------------------------------------------ */

ZTEST(clock_fll, test_clock_wave_get)
{
	cf_req.data[0] = TT_SMC_MSG_CLOCK_WAVE | (CLOCK_WAVE_OP_GET << 8);
	push_msg();

	zassert_equal(cf_rsp.data[0], 0);
	zassert_equal(cf_rsp.data[1], TT_CLK_SCHEME_ZERO_SKEW, "decoded scheme, not raw CNTL");
	zassert_equal(cf_rsp.data[6], CNTL_RESET, "raw CLOCK_WAVE_CNTL");
	zassert_equal(cf_rsp.data[2], MODE_RESET);
	zassert_equal(cf_rsp.data[3], MODE_RESET);
	zassert_equal(cf_rsp.data[4], MODE_RESET);
	zassert_equal(cf_rsp.data[5], MODE_3_RESET);
	zassert_equal(wr_count, 0, "GET must not write anything");
}

/*
 * Regression: GET used to report the raw CLOCK_WAVE_CNTL in data[1] while APPLY
 * read a scheme index from the same slot, so feeding a GET response back into an
 * APPLY silently flipped zero-skew to mesh. Found on a p300a, not by this suite.
 */
ZTEST(clock_fll, test_clock_wave_get_apply_round_trip)
{
	uint32_t scheme;
	uint32_t modes[4];

	cf_req.data[0] = TT_SMC_MSG_CLOCK_WAVE | (CLOCK_WAVE_OP_GET << 8);
	push_msg();
	zassert_equal(cf_rsp.data[0], 0);
	scheme = cf_rsp.data[1];
	for (unsigned int i = 0; i < 4; i++) {
		modes[i] = cf_rsp.data[2 + i];
	}

	/* Hand the GET response straight back to APPLY, unmodified. */
	memset(&cf_req, 0, sizeof(cf_req));
	cf_req.data[0] = TT_SMC_MSG_CLOCK_WAVE | (CLOCK_WAVE_OP_APPLY << 8);
	cf_req.clock_wave.scheme = scheme;
	for (unsigned int i = 0; i < 4; i++) {
		cf_req.clock_wave.mode[i] = modes[i];
	}
	push_msg();

	zassert_equal(cf_rsp.data[0], 0);
	zassert_equal(cf_rsp.data[1], scheme, "APPLY of a GET must not change the scheme");
	zassert_equal(cf_rsp.data[6], CNTL_RESET, "raw CNTL must be back where it started");
	for (unsigned int i = 0; i < 4; i++) {
		zassert_equal(cf_rsp.data[2 + i], modes[i]);
	}
}

ZTEST(clock_fll, test_clock_wave_apply_enables_rows_in_order)
{
	/* Enable both directions on every row, retimer mode 0 (the only
	 * setting guaranteed to work), and select the mesh.
	 */
	cf_req.data[0] = TT_SMC_MSG_CLOCK_WAVE | (CLOCK_WAVE_OP_APPLY << 8);
	cf_req.clock_wave.scheme = TT_CLK_SCHEME_CLOCK_WAVE;
	cf_req.clock_wave.mode[0] = 0;
	cf_req.clock_wave.mode[1] = 0;
	cf_req.clock_wave.mode[2] = 0;
	cf_req.clock_wave.mode[3] = 0;
	cf_req.clock_wave.flags = 0;
	push_msg();

	zassert_equal(cf_rsp.data[0], 0);

	/* Both enables off, then the four mode registers, then the new mux. */
	zassert_equal(wr_count, 6, "unexpected write count %u", wr_count);
	zassert_equal(wr_trace[0].addr, CLOCK_WAVE_CNTL_ADDR);
	zassert_equal(wr_trace[0].val, 0, "both enables must be deasserted first");
	for (unsigned int i = 0; i < 4; i++) {
		zassert_equal(wr_trace[1 + i].addr, MODE_ADDR(i));
		zassert_equal(wr_trace[1 + i].val, 0);
	}
	zassert_equal(wr_trace[5].addr, CLOCK_WAVE_CNTL_ADDR);
	zassert_equal(wr_trace[5].val, 2, "mesh enable only");

	/* Response reflects the new state. */
	zassert_equal(cf_rsp.data[1], TT_CLK_SCHEME_CLOCK_WAVE);
	zassert_equal(cf_rsp.data[6], 2, "raw CLOCK_WAVE_CNTL, mesh enable only");
	zassert_equal(cf_rsp.data[2], 0);
	zassert_equal(cf_rsp.data[5], 0);
}

ZTEST(clock_fll, test_clock_wave_apply_zero_skew)
{
	cf_req.data[0] = TT_SMC_MSG_CLOCK_WAVE | (CLOCK_WAVE_OP_APPLY << 8);
	cf_req.clock_wave.scheme = TT_CLK_SCHEME_ZERO_SKEW;
	push_msg();

	zassert_equal(cf_rsp.data[0], 0);
	zassert_equal(cf_rsp.data[1], TT_CLK_SCHEME_ZERO_SKEW);
	zassert_equal(cf_rsp.data[6], 1, "zero skew enable only");
}

ZTEST(clock_fll, test_clock_wave_apply_skip_modes)
{
	cf_req.data[0] = TT_SMC_MSG_CLOCK_WAVE | (CLOCK_WAVE_OP_APPLY << 8);
	cf_req.clock_wave.scheme = TT_CLK_SCHEME_CLOCK_WAVE;
	cf_req.clock_wave.mode[0] = 0; /* ignored */
	cf_req.clock_wave.flags = CLOCK_WAVE_FLAG_SKIP_MODES;
	push_msg();

	zassert_equal(cf_rsp.data[0], 0);
	zassert_equal(wr_count, 2, "only the two CLOCK_WAVE_CNTL writes");
	zassert_false(addr_was_written(MODE_ADDR(0)));
	zassert_equal(shadow_get(MODE_ADDR(0)), MODE_RESET);
	zassert_equal(cf_rsp.data[1], TT_CLK_SCHEME_CLOCK_WAVE);
	zassert_equal(cf_rsp.data[6], 2);
}

ZTEST(clock_fll, test_clock_wave_apply_skip_scheme)
{
	cf_req.data[0] = TT_SMC_MSG_CLOCK_WAVE | (CLOCK_WAVE_OP_APPLY << 8);
	cf_req.clock_wave.scheme = TT_CLK_SCHEME_CLOCK_WAVE; /* ignored */
	cf_req.clock_wave.mode[0] = 0x00000066;
	cf_req.clock_wave.flags = CLOCK_WAVE_FLAG_SKIP_SCHEME;
	push_msg();

	zassert_equal(cf_rsp.data[0], 0);
	zassert_equal(shadow_get(MODE_ADDR(0)), 0x00000066);
	zassert_equal(cf_rsp.data[1], TT_CLK_SCHEME_ZERO_SKEW, "scheme must be left as it was");
	zassert_equal(cf_rsp.data[6], CNTL_RESET);
}

ZTEST(clock_fll, test_clock_wave_apply_rejects_reserved_bits)
{
	cf_req.data[0] = TT_SMC_MSG_CLOCK_WAVE | (CLOCK_WAVE_OP_APPLY << 8);
	cf_req.clock_wave.scheme = TT_CLK_SCHEME_CLOCK_WAVE;
	cf_req.clock_wave.mode[0] = 0x00000088; /* bits 3 and 7 are reserved */
	push_msg();

	zassert_equal(cf_rsp.data[0], (uint8_t)-EINVAL);
	zassert_equal(wr_count, 0, "a rejected request must not touch the bus");
}

ZTEST(clock_fll, test_clock_wave_apply_rejects_absent_rows_in_mode3)
{
	cf_req.data[0] = TT_SMC_MSG_CLOCK_WAVE | (CLOCK_WAVE_OP_APPLY << 8);
	cf_req.clock_wave.scheme = TT_CLK_SCHEME_CLOCK_WAVE;
	cf_req.clock_wave.mode[3] = 0x00001100; /* MODE_3 carries row 12 only */
	push_msg();

	zassert_equal(cf_rsp.data[0], (uint8_t)-EINVAL);
	zassert_equal(wr_count, 0);
}

ZTEST(clock_fll, test_clock_wave_apply_rejects_bad_scheme_and_flags)
{
	cf_req.data[0] = TT_SMC_MSG_CLOCK_WAVE | (CLOCK_WAVE_OP_APPLY << 8);
	cf_req.clock_wave.scheme = 2;
	push_msg();
	zassert_equal(cf_rsp.data[0], (uint8_t)-EINVAL);
	zassert_equal(wr_count, 0);

	clear_traces();
	memset(&cf_req, 0, sizeof(cf_req));
	cf_req.data[0] = TT_SMC_MSG_CLOCK_WAVE | (CLOCK_WAVE_OP_APPLY << 8);
	cf_req.clock_wave.flags = BIT(7);
	push_msg();
	zassert_equal(cf_rsp.data[0], (uint8_t)-EINVAL);
	zassert_equal(wr_count, 0);
}

ZTEST(clock_fll, test_clock_wave_rejects_bad_op)
{
	cf_req.data[0] = TT_SMC_MSG_CLOCK_WAVE | (0xAA << 8);
	push_msg();

	zassert_equal(cf_rsp.data[0], (uint8_t)-EINVAL);
	zassert_equal(wr_count, 0);
}

ZTEST(clock_fll, test_switch_clk_scheme_still_mux_only)
{
	/* The legacy 0x14 message must keep working, and must keep its
	 * narrow scope: the mux only, never the per-row enables.
	 */
	cf_req.data[0] = TT_SMC_MSG_SWITCH_CLK_SCHEME;
	cf_req.data[1] = TT_CLK_SCHEME_CLOCK_WAVE;
	push_msg();

	zassert_equal(cf_rsp.data[0], 0);
	zassert_equal(shadow_get(CLOCK_WAVE_CNTL_ADDR), 2);
	zassert_equal(shadow_get(MODE_ADDR(0)), MODE_RESET,
		      "0x14 must not touch the row enables");
	zassert_equal(wr_count, 2);
}

/* ------------------------------------------------------------------ */
/* TT_SMC_MSG_FLL_DROOP                                                */
/* ------------------------------------------------------------------ */

ZTEST(clock_fll, test_fll_get)
{
	shadow_find(FLL_ADDR(3))->val = 0xA5A50008;

	cf_req.data[0] = TT_SMC_MSG_FLL_DROOP | (FLL_DROOP_OP_GET << 8);
	cf_req.fll_droop.arg0 = 3;
	push_msg();

	zassert_equal(cf_rsp.data[0], 0);
	zassert_equal(cf_rsp.data[1], 0xA5A50008);
	zassert_equal(cf_rsp.data[2], FLL_ADDR(3));
	zassert_equal(wr_count, 0);
}

ZTEST(clock_fll, test_fll_get_status0_is_readable)
{
	/* Index 6 is FLL_STATUS_0, not an absent FLL_CNTL_6. */
	cf_req.data[0] = TT_SMC_MSG_FLL_DROOP | (FLL_DROOP_OP_GET << 8);
	cf_req.fll_droop.arg0 = 6;
	push_msg();

	zassert_equal(cf_rsp.data[0], 0);
	zassert_equal(cf_rsp.data[1], FLL_STATUS_0_SAMPLE);
	zassert_equal(cf_rsp.data[2], FLL_STATUS_0_ADDR);
	zassert_equal(wr_count, 0);
}

ZTEST(clock_fll, test_fll_rejects_unmapped_and_out_of_range)
{
	/* 0x80020020 is not in the a0 register map; never touch it. */
	cf_req.data[0] = TT_SMC_MSG_FLL_DROOP | (FLL_DROOP_OP_GET << 8);
	cf_req.fll_droop.arg0 = 7;
	push_msg();

	zassert_equal(cf_rsp.data[0], (uint8_t)-ENOTSUP);
	zassert_false(addr_was_read(FLL_ADDR(7)),
		      "must not touch a possibly unmapped address");
	zassert_equal(wr_count, 0);

	clear_traces();
	memset(&cf_req, 0, sizeof(cf_req));
	cf_req.data[0] = TT_SMC_MSG_FLL_DROOP | (FLL_DROOP_OP_GET << 8);
	cf_req.fll_droop.arg0 = 10;
	push_msg();
	zassert_equal(cf_rsp.data[0], (uint8_t)-EINVAL);
}

ZTEST(clock_fll, test_fll_dump)
{
	static const uint8_t implemented[] = {0, 1, 2, 3, 4, 5, 8, 9};

	for (unsigned int i = 0; i < ARRAY_SIZE(implemented); i++) {
		shadow_find(FLL_ADDR(implemented[i]))->val = 0x1000 + implemented[i];
	}

	cf_req.data[0] = TT_SMC_MSG_FLL_DROOP | (FLL_DROOP_OP_DUMP << 8);
	cf_req.fll_droop.arg0 = 0;
	push_msg();

	zassert_equal(cf_rsp.data[0], 0);
	for (unsigned int i = 0; i < 7; i++) {
		zassert_equal(cf_rsp.data[1 + i], 0x1000 + implemented[i],
			      "dump slot %u mismatched", i);
	}
	zassert_false(addr_was_read(FLL_ADDR(6)));
	zassert_false(addr_was_read(FLL_ADDR(7)));

	/* The tail of the list: only FLL_CNTL_9 is left. */
	clear_traces();
	memset(&cf_req, 0, sizeof(cf_req));
	cf_req.data[0] = TT_SMC_MSG_FLL_DROOP | (FLL_DROOP_OP_DUMP << 8);
	cf_req.fll_droop.arg0 = 7;
	push_msg();

	zassert_equal(cf_rsp.data[0], 0);
	zassert_equal(cf_rsp.data[1], 0x1009);
	zassert_equal(cf_rsp.data[2], 0, "slots past the end stay zero");

	/* Past the end of the list. */
	clear_traces();
	memset(&cf_req, 0, sizeof(cf_req));
	cf_req.data[0] = TT_SMC_MSG_FLL_DROOP | (FLL_DROOP_OP_DUMP << 8);
	cf_req.fll_droop.arg0 = 8;
	push_msg();
	zassert_equal(cf_rsp.data[0], (uint8_t)-EINVAL);
}

ZTEST(clock_fll, test_fll_set_masked)
{
	shadow_find(FLL_ADDR(3))->val = 0xFFFF0000;

	/* Set dd_enable (bit 3) without disturbing anything else. */
	cf_req.data[0] = TT_SMC_MSG_FLL_DROOP | (FLL_DROOP_OP_SET << 8);
	cf_req.fll_droop.arg0 = 3;
	cf_req.fll_droop.arg1 = BIT(3);
	cf_req.fll_droop.arg2 = BIT(3);
	push_msg();

	zassert_equal(cf_rsp.data[0], 0);
	zassert_equal(shadow_get(FLL_ADDR(3)), 0xFFFF0008);
	zassert_equal(cf_rsp.data[1], 0xFFFF0008, "value after the write");
	zassert_equal(cf_rsp.data[2], 0xFFFF0000, "value before the write");

	/* And clear it again through the same mask. */
	clear_traces();
	memset(&cf_req, 0, sizeof(cf_req));
	cf_req.data[0] = TT_SMC_MSG_FLL_DROOP | (FLL_DROOP_OP_SET << 8);
	cf_req.fll_droop.arg0 = 3;
	cf_req.fll_droop.arg1 = 0;
	cf_req.fll_droop.arg2 = BIT(3);
	push_msg();

	zassert_equal(cf_rsp.data[0], 0);
	zassert_equal(shadow_get(FLL_ADDR(3)), 0xFFFF0000);
}

ZTEST(clock_fll, test_fll_set_zero_mask_writes_whole_register)
{
	shadow_find(FLL_ADDR(1))->val = 0xFFFFFFFF;

	cf_req.data[0] = TT_SMC_MSG_FLL_DROOP | (FLL_DROOP_OP_SET << 8);
	cf_req.fll_droop.arg0 = 1;
	cf_req.fll_droop.arg1 = 0x00320064; /* droop_threshold 0x32, droop2 0x64 */
	cf_req.fll_droop.arg2 = 0;
	push_msg();

	zassert_equal(cf_rsp.data[0], 0);
	zassert_equal(shadow_get(FLL_ADDR(1)), 0x00320064);
}

ZTEST(clock_fll, test_fll_set_rejects_status0_and_unmapped)
{
	cf_req.data[0] = TT_SMC_MSG_FLL_DROOP | (FLL_DROOP_OP_SET << 8);
	cf_req.fll_droop.arg0 = 6;
	cf_req.fll_droop.arg1 = 1;
	push_msg();

	zassert_equal(cf_rsp.data[0], (uint8_t)-EACCES, "FLL_STATUS_0 is read-only");
	zassert_false(addr_was_written(FLL_STATUS_0_ADDR));
	zassert_equal(wr_count, 0);

	clear_traces();
	memset(&cf_req, 0, sizeof(cf_req));
	cf_req.data[0] = TT_SMC_MSG_FLL_DROOP | (FLL_DROOP_OP_SET << 8);
	cf_req.fll_droop.arg0 = 7;
	cf_req.fll_droop.arg1 = 1;
	push_msg();

	zassert_equal(cf_rsp.data[0], (uint8_t)-ENOTSUP);
	zassert_false(addr_was_written(FLL_ADDR(7)));
	zassert_equal(wr_count, 0);
}

ZTEST(clock_fll, test_fll_ro_count)
{
	cf_req.data[0] = TT_SMC_MSG_FLL_DROOP | (FLL_DROOP_OP_RO_COUNT << 8);
	cf_req.fll_droop.arg0 = 4; /* reads */
	cf_req.fll_droop.arg1 = 3; /* discard */
	push_msg();

	zassert_equal(cf_rsp.data[0], 0);
	zassert_equal(cf_rsp.data[1], 18, "mean of 16 and 20");
	zassert_equal(cf_rsp.data[2], 16, "min");
	zassert_equal(cf_rsp.data[3], 20, "max");
	zassert_equal(cf_rsp.data[4], 8, "two samples per read");
	zassert_equal(status0_reads, 7, "discarded reads must actually happen");
	zassert_equal(wr_count, 0);
}

ZTEST(clock_fll, test_fll_ro_count_rejects_other_debug_modes)
{
	/* debug_data_mode != 0 packs 8/4/1-bit values; the 16-bit unpack would
	 * return a plausible-looking wrong number.
	 */
	shadow_find(FLL_ADDR(9))->val = BIT(16);

	cf_req.data[0] = TT_SMC_MSG_FLL_DROOP | (FLL_DROOP_OP_RO_COUNT << 8);
	push_msg();

	zassert_equal(cf_rsp.data[0], (uint8_t)-ENOTSUP);
	zassert_equal(status0_reads, 0);
}

ZTEST(clock_fll, test_fll_sample_nominal_pulses)
{
	shadow_find(FLL_ADDR(3))->val = BIT(3); /* dd_enable already set */

	cf_req.data[0] = TT_SMC_MSG_FLL_DROOP | (FLL_DROOP_OP_SAMPLE_NOMINAL << 8);
	push_msg();

	zassert_equal(cf_rsp.data[0], 0);
	zassert_equal(wr_count, 2);
	zassert_equal(wr_trace[0].addr, FLL_ADDR(3));
	zassert_equal(wr_trace[0].val, BIT(3) | BIT(0));
	zassert_equal(wr_trace[1].addr, FLL_ADDR(3));
	zassert_equal(wr_trace[1].val, BIT(3), "unrelated bits must survive");
	zassert_equal(cf_rsp.data[1], BIT(3));
}

ZTEST(clock_fll, test_fll_pulse_asserts_then_deasserts)
{
	/* Pre-set an unrelated bit to confirm it survives the pulse. */
	shadow_find(FLL_ADDR(0))->val = BIT(8); /* dd_fbdiv_ctrl_en */

	cf_req.data[0] = TT_SMC_MSG_FLL_DROOP | (FLL_DROOP_OP_PULSE << 8);
	cf_req.fll_droop.arg0 = FLL_DROOP_TRIGGER_DROOP | FLL_DROOP_TRIGGER_MANUAL_DROOP;
	cf_req.fll_droop.arg1 = 4;
	push_msg();

	zassert_equal(cf_rsp.data[0], 0);

	/* FLL_CNTL_0 asserted with force_droop_detect, then cleared. */
	zassert_equal(wr_count, 4, "two asserts and two deasserts");
	zassert_equal(wr_trace[0].addr, FLL_ADDR(0));
	zassert_equal(wr_trace[0].val, BIT(8) | BIT(5));
	zassert_equal(wr_trace[1].addr, FLL_ADDR(9));
	zassert_equal(wr_trace[1].val, BIT(18));
	zassert_equal(wr_trace[2].addr, FLL_ADDR(0));
	zassert_equal(wr_trace[2].val, BIT(8), "unrelated bits must survive");
	zassert_equal(wr_trace[3].addr, FLL_ADDR(9));
	zassert_equal(wr_trace[3].val, 0);

	zassert_equal(cf_rsp.data[1], BIT(8));
	zassert_equal(cf_rsp.data[2], 0);
}

ZTEST(clock_fll, test_fll_pulse_rejects_bad_triggers)
{
	cf_req.data[0] = TT_SMC_MSG_FLL_DROOP | (FLL_DROOP_OP_PULSE << 8);
	cf_req.fll_droop.arg0 = 0;
	push_msg();
	zassert_equal(cf_rsp.data[0], (uint8_t)-EINVAL);
	zassert_equal(wr_count, 0);

	clear_traces();
	memset(&cf_req, 0, sizeof(cf_req));
	cf_req.data[0] = TT_SMC_MSG_FLL_DROOP | (FLL_DROOP_OP_PULSE << 8);
	cf_req.fll_droop.arg0 = BIT(9);
	push_msg();
	zassert_equal(cf_rsp.data[0], (uint8_t)-EINVAL);
	zassert_equal(wr_count, 0);
}

ZTEST(clock_fll, test_fll_rejects_bad_op)
{
	cf_req.data[0] = TT_SMC_MSG_FLL_DROOP | (0xAA << 8);
	push_msg();

	zassert_equal(cf_rsp.data[0], (uint8_t)-EINVAL);
	zassert_equal(wr_count, 0);
}

ZTEST_SUITE(clock_fll, NULL, NULL, clock_fll_before, NULL, NULL);
