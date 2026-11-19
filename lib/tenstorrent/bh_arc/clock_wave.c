/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "clock_wave.h"

#include <errno.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <tenstorrent/smc_msg.h>
#include <tenstorrent/msgqueue.h>

#include "timer.h"
#include "reg.h"

LOG_MODULE_REGISTER(clock_wave, CONFIG_TT_APP_LOG_LEVEL);

#define PLL_CNTL_WRAPPER_CLOCK_WAVE_CNTL_REG_ADDR 0x80020038

/*
 * reset_unit.CLOCK_WAVE_CONTROL_MODE_0..3: the per-row, per-direction enables
 * for the clock wave retimer. These are the second layer of the clock wave
 * configuration, independent of the CLOCK_WAVE_CNTL mux above.
 *
 * MODE_0 covers NOC rows 0-3, MODE_1 rows 4-7, MODE_2 rows 8-11, and MODE_3
 * holds row 12 in its low byte. Each row byte is:
 *
 *   [0]   s_dis    south direction retimer disable
 *   [2:1] s_mode   south direction retimer mode
 *   [3]   reserved
 *   [4]   n_dis    north direction retimer disable
 *   [6:5] n_mode   north direction retimer mode
 *   [7]   reserved
 *
 * Reset default is 0x11 per row byte, i.e. both directions disabled on every
 * row, which is why selecting the mesh mux on its own changes nothing.
 */
#define RESET_UNIT_CLOCK_WAVE_CONTROL_MODE_0_REG_ADDR 0x80030B00
#define CLOCK_WAVE_CONTROL_MODE_COUNT                 4
#define CLOCK_WAVE_CONTROL_MODE_REG_ADDR(i)                                                        \
	(RESET_UNIT_CLOCK_WAVE_CONTROL_MODE_0_REG_ADDR + 4U * (i))

/* MODE_3 is the odd one out: it carries row 12 alone, in its low byte. */
#define CLOCK_WAVE_LAST_MODE_REG 3

/* Writable bits of one row byte: s_dis, s_mode, n_dis, n_mode. */
#define CLOCK_WAVE_ROW_VALID_MASK 0x77U
/* Only the retimer mode fields, which are the unvalidated ones. See below. */
#define CLOCK_WAVE_ROW_MODE_MASK 0x66U

#define CLOCK_WAVE_CONTROL_MODE_VALID(i)                                                           \
	(((i) == CLOCK_WAVE_LAST_MODE_REG) ? (uint32_t)CLOCK_WAVE_ROW_VALID_MASK                   \
					   : 0x77777777U)
#define CLOCK_WAVE_CONTROL_MODE_MODE_BITS(i)                                                       \
	(((i) == CLOCK_WAVE_LAST_MODE_REG) ? (uint32_t)CLOCK_WAVE_ROW_MODE_MASK : 0x66666666U)

typedef struct {
	uint32_t aiclk_zsk_enb: 1;
	uint32_t aiclk_mesh_enb: 1;
} PLL_CNTL_WRAPPER_CLOCK_WAVE_CNTL_reg_t;

typedef union {
	uint32_t val;
	PLL_CNTL_WRAPPER_CLOCK_WAVE_CNTL_reg_t f;
} PLL_CNTL_WRAPPER_CLOCK_WAVE_CNTL_reg_u;

#define PLL_CNTL_WRAPPER_CLOCK_WAVE_CNTL_REG_DEFAULT 0x00000001

static uint32_t clk_scheme_to_cntl(enum tt_clk_scheme clk_scheme)
{
	PLL_CNTL_WRAPPER_CLOCK_WAVE_CNTL_reg_u clock_wave_cntl;

	clock_wave_cntl.val = 0;
	if (clk_scheme == TT_CLK_SCHEME_CLOCK_WAVE) {
		clock_wave_cntl.f.aiclk_mesh_enb = 1;
		clock_wave_cntl.f.aiclk_zsk_enb = 0;
	} else {
		clock_wave_cntl.f.aiclk_mesh_enb = 0;
		clock_wave_cntl.f.aiclk_zsk_enb = 1;
	}

	return clock_wave_cntl.val;
}

/*
 * Drive CLOCK_WAVE_CNTL through the mandatory both-enables-off window.
 *
 * The hardware requires both enables deasserted for at least 10 REFCLK cycles
 * before the other one is asserted. AICLK is gated during that window, so this
 * can only be done from the ARC (clocked by ARCCLK): a host write over the NOC
 * would cut off the very path that is meant to restore the clock.
 *
 * If mode_regs is non-NULL the row mode registers are programmed inside the
 * window, while the retimers are quiesced.
 */
static void apply_clock_wave_cntl(uint32_t cntl, const uint32_t *mode_regs)
{
	WriteReg(PLL_CNTL_WRAPPER_CLOCK_WAVE_CNTL_REG_ADDR, 0);
	Wait(10); /* both enables are off for 10 refclk cycles */

	if (mode_regs != NULL) {
		for (unsigned int i = 0; i < CLOCK_WAVE_CONTROL_MODE_COUNT; i++) {
			WriteReg(CLOCK_WAVE_CONTROL_MODE_REG_ADDR(i), mode_regs[i]);
		}
	}

	WriteReg(PLL_CNTL_WRAPPER_CLOCK_WAVE_CNTL_REG_ADDR, cntl);
	Wait(10); /* wait for 10 refclk cycles for aiclk to stablize */
}

static void switch_clk_scheme(enum tt_clk_scheme clk_scheme)
{
	apply_clock_wave_cntl(clk_scheme_to_cntl(clk_scheme), NULL);
}

static void read_clock_wave_state(struct response *response)
{
	response->data[1] = ReadReg(PLL_CNTL_WRAPPER_CLOCK_WAVE_CNTL_REG_ADDR);
	for (unsigned int i = 0; i < CLOCK_WAVE_CONTROL_MODE_COUNT; i++) {
		response->data[2 + i] = ReadReg(CLOCK_WAVE_CONTROL_MODE_REG_ADDR(i));
	}
}

/**
 * @brief Handler for TT_SMC_MSG_SWITCH_CLK_SCHEME messages
 *
 * @details Switches the clock scheme configuration. This affects the AI clock
 *          distribution and timing.
 *
 *          This only drives the CLOCK_WAVE_CNTL mux. Every NOC row's retimer is
 *          disabled out of reset, so on a stock part this selects the mesh
 *          without any row actually using it. Use @ref TT_SMC_MSG_CLOCK_WAVE to
 *          reach the per-row enables as well.
 *
 * @param request Pointer to the host request message, use request->switch_clk_scheme for structured
 *                access
 * @param response Pointer to the response message to be sent back to host
 *
 * @return 0 on success
 * @return non-zero on error
 *
 * @see switch_clk_scheme_rqst
 */
static uint8_t switch_clk_scheme_handler(const union request *request, struct response *response)
{
	uint32_t clk_scheme = request->switch_clk_scheme.scheme;

	switch_clk_scheme(clk_scheme);
	return 0;
}
REGISTER_MESSAGE(TT_SMC_MSG_SWITCH_CLK_SCHEME, switch_clk_scheme_handler);

static uint8_t clock_wave_apply(const struct clock_wave_rqst *rqst, struct response *response)
{
	uint32_t mode_regs[CLOCK_WAVE_CONTROL_MODE_COUNT];
	bool write_modes = (rqst->flags & CLOCK_WAVE_FLAG_SKIP_MODES) == 0;
	bool write_scheme = (rqst->flags & CLOCK_WAVE_FLAG_SKIP_SCHEME) == 0;
	uint32_t cntl;

	if (rqst->flags & ~(uint32_t)(CLOCK_WAVE_FLAG_SKIP_MODES | CLOCK_WAVE_FLAG_SKIP_SCHEME)) {
		return (uint8_t)-EINVAL;
	}

	if (write_scheme) {
		if (rqst->scheme != TT_CLK_SCHEME_ZERO_SKEW &&
		    rqst->scheme != TT_CLK_SCHEME_CLOCK_WAVE) {
			return (uint8_t)-EINVAL;
		}
		cntl = clk_scheme_to_cntl(rqst->scheme);
	} else {
		cntl = ReadReg(PLL_CNTL_WRAPPER_CLOCK_WAVE_CNTL_REG_ADDR);
	}

	if (write_modes) {
		uint32_t mode_bits_set = 0;

		for (unsigned int i = 0; i < CLOCK_WAVE_CONTROL_MODE_COUNT; i++) {
			if (rqst->mode[i] & ~CLOCK_WAVE_CONTROL_MODE_VALID(i)) {
				/* Reserved bits, or rows that MODE_3 does not carry. */
				return (uint8_t)-EINVAL;
			}
			mode_regs[i] = rqst->mode[i];
			mode_bits_set |= rqst->mode[i] & CLOCK_WAVE_CONTROL_MODE_MODE_BITS(i);
		}

		/*
		 * s_mode/n_mode values other than 0 are UNVALIDATED. The clock
		 * wave retimer's negedge flops were not ensured on non-Tensix
		 * blocks and may not work; only mode 0, the zero-skew retimer
		 * setting, is guaranteed. Allowed here for characterisation,
		 * never enabled by default.
		 */
		if (mode_bits_set != 0) {
			LOG_WRN("clock wave retimer mode != 0 requested; "
				"unvalidated on non-Tensix blocks");
		}
	}

	apply_clock_wave_cntl(cntl, write_modes ? mode_regs : NULL);
	read_clock_wave_state(response);

	return 0;
}

/**
 * @brief Handler for TT_SMC_MSG_CLOCK_WAVE messages
 *
 * @details Reads back or programs both layers of the AICLK clock wave
 *          configuration: the CLOCK_WAVE_CNTL distribution mux and the
 *          reset_unit CLOCK_WAVE_CONTROL_MODE_0..3 per-row retimer enables.
 *
 *          The whole sequence runs on the ARC, which is clocked by ARCCLK, so
 *          gating AICLK does not cut off the path performing the update.
 *
 * @param request Pointer to the host request message, use request->clock_wave for structured access
 * @param response Pointer to the response message to be sent back to host
 *
 * @return 0 on success
 * @return non-zero on error
 *
 * @see clock_wave_rqst
 */
static uint8_t clock_wave_handler(const union request *request, struct response *response)
{
	const struct clock_wave_rqst *rqst = &request->clock_wave;

	switch (rqst->op) {
	case CLOCK_WAVE_OP_GET:
		read_clock_wave_state(response);
		return 0;
	case CLOCK_WAVE_OP_APPLY:
		return clock_wave_apply(rqst, response);
	default:
		return (uint8_t)-EINVAL;
	}
}
REGISTER_MESSAGE(TT_SMC_MSG_CLOCK_WAVE, clock_wave_handler);
