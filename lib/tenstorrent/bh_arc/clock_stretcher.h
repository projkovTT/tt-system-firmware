/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TENSTORRENT_BH_ARC_CLOCK_STRETCHER_H_
#define TENSTORRENT_BH_ARC_CLOCK_STRETCHER_H_

#include <stdint.h>

#include <zephyr/kernel.h>

/**
 * @brief Re-point the FLL droop detector's nominal RO count to a VCORE operating
 *        point using the calibrated fit
 *
 * Recomputes dd_fw_nominal_vdd from the stored RO-count-vs-VCORE fit and writes it;
 * Intended to be called from the DVFS path on each VCORE change so the detector tracks the operating voltage. 
 * Has a no-op guard for the case where the fit is invalid/unavailable
 *
 * @param vcore_mv Target core voltage in millivolts.
 */
#ifdef CONFIG_TT_BH_ARC_CLOCK_STRETCHER
void clock_stretcher_repoint(uint32_t vcore_mv);
#else
/* clock_stretcher.c is not built without the feature; voltage.c calls this on
 * every VCORE change regardless.
 */
static inline void clock_stretcher_repoint(uint32_t vcore_mv)
{
	ARG_UNUSED(vcore_mv);
}
#endif

#endif /* TENSTORRENT_BH_ARC_CLOCK_STRETCHER_H_ */
