.. _ttzp_clock_wave_fll:

Clock Wave and FLL Droop Detector Control
#########################################

Two ARC messages expose the AICLK clock-distribution scheme and the FLL droop
detector (clock stretcher) to the host, for di/dt and supply-noise
characterisation.

Why this has to run on the ARC
==============================

Both features live behind registers that the host can only reach over the NOC,
through the ARC tile. AICLK clocks the NOC, and the clock-wave switch sequence
requires AICLK to be gated for ten REFCLK cycles. A host write that gates AICLK
cuts off the very transaction that would restore it: measured on a p300a, the
second write fails, the Tensixes are left in reset, and both ASICs lose PCIe
until ``tt-smi -r``.

The ARC runs on ARCCLK, not AICLK, so it survives the window. Both handlers
therefore perform the whole sequence on the ARC and only report results back.

``TT_SMC_MSG_CLOCK_WAVE`` (0x1F)
================================

Covers both layers of the clock-wave configuration:

* ``pll_cntl_wrapper.CLOCK_WAVE_CNTL`` (0x80020038), which selects zero-skew
  (bit 0) versus mesh (bit 1). This is the layer that ``TT_SMC_MSG_SWITCH_CLK_SCHEME``
  (0x14) already drives.
* ``reset_unit.CLOCK_WAVE_CONTROL_MODE_0..3`` (0x80030B00/B04/B08/B0C), the
  per-row, per-direction retimer enables. Nothing in firmware touched these
  before. Reset default is ``0x11`` per row byte, i.e. **every row's retimer is
  disabled**, which is why selecting the mesh mux on its own has no observable
  effect.

MODE_0 covers NOC rows 0-3, MODE_1 rows 4-7, MODE_2 rows 8-11, and MODE_3 holds
row 12 in its low byte. Each row byte is ``[0] s_dis``, ``[2:1] s_mode``,
``[4] n_dis``, ``[6:5] n_mode``; bits 3 and 7 are reserved and rejected.

.. warning::
   ``s_mode``/``n_mode`` values other than 0 are **unvalidated**. The clock-wave
   retimer's negedge flops were not ensured on non-Tensix blocks and may not
   work; only setting 0, the ZSK retimer setting, is guaranteed. They are
   settable here for characterisation and are never enabled by default.

Request
-------

====== ========================================================================
Word   Contents
====== ========================================================================
0      ``0x1F`` | ``op << 8``. ``op`` 0 = GET, 1 = APPLY.
1      Clock scheme: 0 = zero skew, 1 = clock wave. APPLY only.
2-5    ``CLOCK_WAVE_CONTROL_MODE_0..3``. APPLY only.
6      Flags: bit 0 = skip the mode registers, bit 1 = keep the current scheme.
====== ========================================================================

Response
--------

====== ========================================================================
Word   Contents
====== ========================================================================
0      Return code: ``0``, else ``(uint8_t)-errno`` (``-EINVAL`` reads as 234).
1      Clock scheme after the operation, in the same 0/1 encoding word 1 of the
       request takes, so a GET response can be fed straight back into an APPLY.
2-5    ``CLOCK_WAVE_CONTROL_MODE_0..3`` after the operation.
6      Raw ``CLOCK_WAVE_CNTL`` after the operation.
====== ========================================================================

.. note::
   Word 6 is a pair of enables -- ``0x1`` zero skew, ``0x2`` mesh -- not the
   scheme index, and must not be passed back as the scheme. Word 1 is the one
   that round-trips. On a stock part a GET returns scheme ``0`` with word 6
   reading ``0x1``.

APPLY always runs the full safe sequence: deassert both enables, wait, program
the mode registers (unless skipped), assert the requested enable, wait. A
request that fails validation touches no registers at all.

``TT_SMC_MSG_FLL_DROOP`` (0x24)
===============================

Read/write access to the FLL droop detector in ``pll_cntl_wrapper``. Nothing is
configured or enabled at boot; this message is the only thing that touches these
registers.

Register indices follow the offset: index *n* is at ``0x80020004 + 4 * n``.

.. list-table::
   :header-rows: 1
   :widths: 10 20 70

   * - Index
     - Register
     - Notes
   * - 0-5
     - FLL_CNTL_0-5
     - Read/write
   * - 6
     - FLL_STATUS_0
     - RO counter readout. Read-only; reads consume samples.
   * - 7
     - --
     - Not in the a0 register map. Refused with ``-ENOTSUP``.
   * - 8-9
     - FLL_CNTL_8-9
     - Read/write

Operations, selected by ``op`` in bits 15:8 of word 0:

.. list-table::
   :header-rows: 1
   :widths: 8 24 68

   * - ``op``
     - Name
     - Arguments and response
   * - 0
     - GET
     - w1 = index. Response: w1 value, w2 address.
   * - 1
     - DUMP
     - w1 = start index into {0,1,2,3,4,5,8,9}. Response: w1-w7 = up to seven
       consecutive FLL_CNTL registers, zero past the end. ``start=0`` then
       ``start=7`` covers all eight. FLL_STATUS_0 is excluded because reading
       it consumes samples.
   * - 2
     - SET
     - w1 = index, w2 = value, w3 = mask (0 means all 32 bits). Response:
       w1 value after the write, w2 value before.
   * - 3
     - PULSE
     - w1 = trigger bitmask, w2 = hold time in REFCLK cycles (0 = 10, capped
       at 50000 = 1 ms). Asserts the bits, waits, then deasserts only those
       bits. Response: w1 = FLL_CNTL_0, w2 = FLL_CNTL_9 afterwards.
   * - 4
     - RO_COUNT
     - w1 = reads to average (0 = 32, max 64), w2 = reads to discard first
       (0 = 10, max 64). Response: w1 mean, w2 min, w3 max, w4 sample count.
       Refused with ``-ENOTSUP`` unless ``debug_data_mode`` is 0.
   * - 5
     - SAMPLE_NOMINAL
     - Pulses ``FLL_CNTL_3[0] dd_force_sample_nominal_vdd``. Response:
       w1 = FLL_CNTL_3 afterwards.

PULSE trigger bits: 0 = ``force_droop_detect``, 1 = ``force_droop2_detect``,
2 = ``force_spike_detect``, 3 = ``manual_spike``, 4 = ``manual_droop``. These
drive the response without a real supply event, which is the first bring-up
test worth running.

.. note::
   The detector compares each RO snapshot against a held nominal count, and that
   nominal has to track VCORE. Either let the hardware latch it
   (``dd_use_nominal_vdd``, re-latched with SAMPLE_NOMINAL after a voltage
   change) or supply it from firmware (``dd_use_fw_nominal_vdd`` plus
   ``FLL_CNTL_5.dd_fw_nominal_vdd``). Setting ``dd_enable`` against a stale
   nominal will either stretch permanently or never stretch at all.

Interaction with the clock stretcher calibration
================================================

``CONFIG_TT_BH_ARC_CLOCK_STRETCHER`` (default ``n``) enables a second owner of
these registers: ``lib/tenstorrent/bh_arc/clock_stretcher.c`` calibrates the
detector's nominal RO count against VCORE at boot, persists the fit to the
``clkstr`` SPIROM partition, and exposes it through ``TT_SMC_MSG_GET_CLK_STRETCHER_FIT``
(0xC8) and ``TT_SMC_MSG_RECALIBRATE_CLK_STRETCHER`` (0xC9).

With it **off**, which is the default, nothing in firmware touches the FLL and
``TT_SMC_MSG_FLL_DROOP`` has sole ownership. Every field reads back zero on a
stock part.

With it **on**, two things will overwrite values set through 0x24:

* ``apply_static_config()`` programs FLL_CNTL_0-5, 8 and 9 during init.
* ``clock_stretcher_repoint()`` runs from ``VoltageChange()`` on **every** DVFS
  voltage step. It rewrites ``FLL_CNTL_5.dd_fw_nominal_vdd`` and forces
  ``FLL_CNTL_3`` to ``dd_use_fw_nominal_vdd=1``, ``dd_use_nominal_vdd=0``,
  ``dd_enable=1``.

So for a free-running experiment either leave the Kconfig off, or pin the
operating point first with ``TT_SMC_MSG_FORCE_VDD`` (0x39) and
``TT_SMC_MSG_FORCE_AICLK`` (0x33) so no voltage change fires. ``SAMPLE_NOMINAL``
and a hand-set ``dd_fw_nominal_vdd`` are the two ways to own the nominal
yourself; see the note above on why it has to track VCORE.

Operating point
---------------

``apply_static_config()`` carries the galaxy starting point for the three fields
that set how hard and how long the detector stretches:

=================== ======= ==================================================
Field               Value   Notes
=================== ======= ==================================================
``droop_threshold`` 15      RO counts below nominal before droop fires
``droop_fbdiv_adj`` 5       subtracted from the PLL feedback divider
``droop_time``      500     stretch duration, in ``ddclk`` periods
=================== ======= ==================================================

These pair with 10 mV of VF-curve guardband
(``CLK_STRETCHER_VOLTAGE_MARGIN_MV`` in ``vf_curve.c``): the detector is meant to
cover the transients that the old process-based margin paid for statically, so
that margin stays disabled.

Two things are worth knowing before reading results against them:

* The three fields above are retunable at runtime with ``TT_SMC_MSG_FLL_DROOP``
  (0x24) ``SET``, so a sweep needs no rebuild. The 10 mV margin is build-time
  only -- there is no margin message, and ``chip_limits.voltage_margin`` from the
  SPI fw table is deliberately *not* added on the process-VF-curve path, so the
  total guardband there is exactly 10 mV whatever the board table says.
* ``vf_curve.c`` is **not** behind ``CONFIG_TT_BH_ARC_CLOCK_STRETCHER``, so the
  10 mV applies to the stretcher-off image too. That image therefore runs with
  less static guardband than a stock build and no droop protection, which is a
  useful baseline but not a safe default.

None of these five numbers has been validated on silicon.

Building and flashing
=====================

Standard SMC build; see :ref:`ttzp_getting_started` for workspace setup.

.. code-block:: shell

   west build --sysbuild -p -b tt_blackhole@p300a/tt_blackhole/smc app/smc -- -DCONFIG_SHELL=y
   west flash -r tt_flash --force

Replace ``p300a`` with the board revision under test. No Kconfig option gates
either message: both handlers are in ``bh_arc_mission`` and are present in any
non-recovery image. The boot-time calibration above is separate and is off
unless ``CONFIG_TT_BH_ARC_CLOCK_STRETCHER=y`` is added to the build.

Driving it
==========

Any path that can post an eight-word request works. ``tt msg`` takes the raw
words, starting with word 0, so on the bench::

   tt msg 0x1f                  # clock wave GET
   tt msg 0x11f 1 0 0 0 0 0     # APPLY: mesh, every row enabled, retimer mode 0
   tt msg 0x11f 0 0x11111111 0x11111111 0x11111111 0x11 0  # back to reset state
   tt msg 0x124 0               # FLL DUMP, FLL_CNTL_0..5 and _8
   tt msg 0x124 7               # FLL DUMP, FLL_CNTL_9
   tt msg 0x324 1 100           # force_droop_detect for 100 REFCLK cycles
   tt msg 0x424                 # averaged RO count

From the host, use a message-queue utility that sends the full eight words
(``pyluwen``'s ``arc_msg_buf`` or the t6py equivalent); the two-argument
``arc_msg`` form only reaches words 1 and 2.

Return codes are ``(uint8_t)-errno``: 234 for ``-EINVAL``, 243 for ``-EACCES``
(a write to FLL_STATUS_0). ``-ENOTSUP`` depends on the libc's errno value, so
treat any other non-zero value as a refusal rather than matching on a number.

Unit tests
==========

``tests/lib/tenstorrent/bh_arc/src/clock_fll.c`` models the registers involved
and asserts on the exact order of bus accesses, which is what makes the
clock-wave sequence safe. Run with twister on ``native_sim``.
