#!/usr/bin/env python3
# Copyright (c) 2026 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0
"""Drive TT_SMC_MSG_CLOCK_WAVE (0x1F) and TT_SMC_MSG_FLL_DROOP (0x24) from the host.

Both messages carry their sub-operation in word 0 bits [15:8], so word 0 is
``msg_id | (op << 8)``. t6py's own ``scripts/tensix_sm/smc/msg.py`` cannot send
that: its ``--msg-id`` is parsed as an 8-bit value, so it can only ever reach
op 0. ``msg_queue.send_message()`` underneath has no such limit and takes the
full 32-bit word, which is what this script uses.

Run it inside t6py's environment::

    cd <syseng>/src/t6ifc/t6py && source bin/venv-activate my-env
    python <this script> clock-wave get

See doc/develop/guides/clock_wave_fll.rst for the wire format.
"""
import argparse
import sys

from tenstorrent.chip import Chip
from tenstorrent.chip.smc import QueueIndex

TT_SMC_MSG_CLOCK_WAVE = 0x1F
TT_SMC_MSG_FLL_DROOP = 0x24

CLOCK_WAVE_OP_GET, CLOCK_WAVE_OP_APPLY = 0, 1
(FLL_OP_GET, FLL_OP_DUMP, FLL_OP_SET, FLL_OP_PULSE, FLL_OP_RO_COUNT,
 FLL_OP_SAMPLE_NOMINAL) = range(6)

SCHEME_NAMES = {0: "zero-skew", 1: "mesh (clock wave)"}
FLL_IMPLEMENTED = [0, 1, 2, 3, 4, 5, 8, 9]
FLL_CNTL_0_ADDR = 0x80020004

TRIGGERS = {
    "droop": 1 << 0,
    "droop2": 1 << 1,
    "spike": 1 << 2,
    "manual-spike": 1 << 3,
    "manual-droop": 1 << 4,
}

# Handlers return uint8_t, so a negative errno is truncated. ENOTSUP is 134 on
# this libc and therefore reaches the host as 122, which is not recoverable as
# a negative number -- it is listed explicitly rather than derived.
ERRNO_NAMES = {234: "-EINVAL", 243: "-EACCES", 122: "-ENOTSUP", 239: "-ENODEV"}


class Smc:
    def __init__(self, interface="pci", queue="tools"):
        chip = Chip.create_chip("blackhole", interface, "direct")
        self.queue = chip.tensix_sm.smc.msg_queue(getattr(QueueIndex, queue))

    def send(self, msg_id, op=0, args=()):
        request = [msg_id | (op << 8)] + list(args)
        if len(request) > 8:
            raise ValueError("request is at most 8 words")
        response = list(self.queue.send_message(request))
        code = response[0] & 0xFF
        if code != 0:
            name = ERRNO_NAMES.get(code, "unknown")
            raise SystemExit("request failed: status {} ({})".format(code, name))
        return response


def show_clock_wave(rsp):
    print("scheme          : {} ({})".format(rsp[1], SCHEME_NAMES.get(rsp[1], "?")))
    print("CLOCK_WAVE_CNTL : 0x{:08x}  (raw enables, NOT the scheme index)".format(rsp[6]))
    for i in range(4):
        rows = "row 12" if i == 3 else "rows {}-{}".format(i * 4, i * 4 + 3)
        print("CONTROL_MODE_{}  : 0x{:08x}  ({})".format(i, rsp[2 + i], rows))


def cmd_clock_wave(smc, args):
    if args.op == "get":
        show_clock_wave(smc.send(TT_SMC_MSG_CLOCK_WAVE, CLOCK_WAVE_OP_GET))
        return

    current = smc.send(TT_SMC_MSG_CLOCK_WAVE, CLOCK_WAVE_OP_GET)
    scheme = current[1] if args.scheme is None else args.scheme
    modes = list(current[2:6])
    for i, value in enumerate(args.mode or []):
        modes[i] = value
    flags = (1 if args.skip_modes else 0) | (2 if args.skip_scheme else 0)

    if any((m & 0x66666666) for m in modes[:3]) or (modes[3] & 0x66):
        print("warning: retimer mode != 0 is UNVALIDATED on non-Tensix blocks",
              file=sys.stderr)

    show_clock_wave(smc.send(TT_SMC_MSG_CLOCK_WAVE, CLOCK_WAVE_OP_APPLY,
                             [scheme] + modes + [flags]))


def cmd_fll(smc, args):
    if args.op == "dump":
        rsp = smc.send(TT_SMC_MSG_FLL_DROOP, FLL_OP_DUMP, [args.start])
        for i, idx in enumerate(FLL_IMPLEMENTED[args.start:args.start + 7]):
            print("FLL_CNTL_{:<2} (0x{:08x}) = 0x{:08x}"
                  .format(idx, FLL_CNTL_0_ADDR + 4 * idx, rsp[1 + i]))
    elif args.op == "get":
        rsp = smc.send(TT_SMC_MSG_FLL_DROOP, FLL_OP_GET, [args.index])
        print("0x{:08x} = 0x{:08x}".format(rsp[2], rsp[1]))
    elif args.op == "set":
        rsp = smc.send(TT_SMC_MSG_FLL_DROOP, FLL_OP_SET,
                       [args.index, args.value, args.mask])
        print("old = 0x{:08x}\nnew = 0x{:08x}".format(rsp[2], rsp[1]))
    elif args.op == "pulse":
        bits = 0
        for name in args.trigger:
            bits |= TRIGGERS[name]
        smc.send(TT_SMC_MSG_FLL_DROOP, FLL_OP_PULSE, [bits, args.cycles])
        print("pulsed {} for {} REFCLK cycles".format(",".join(args.trigger), args.cycles))
    elif args.op == "ro-count":
        rsp = smc.send(TT_SMC_MSG_FLL_DROOP, FLL_OP_RO_COUNT, [args.reads, args.discard])
        print("mean={} min={} max={} samples={}".format(rsp[1], rsp[2], rsp[3], rsp[4]))
    elif args.op == "sample-nominal":
        rsp = smc.send(TT_SMC_MSG_FLL_DROOP, FLL_OP_SAMPLE_NOMINAL)
        print("nominal VDD sample pulsed; FLL_CNTL_3 = 0x{:08x}".format(rsp[1]))


def auto_int(text):
    return int(text, 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--interface", default="pci")
    parser.add_argument("--queue", default="tools",
                        help="message queue name (default: tools)")
    sub = parser.add_subparsers(dest="cmd", required=True)

    cw = sub.add_parser("clock-wave", help="AICLK clock wave (0x1F)")
    cw.add_argument("op", choices=["get", "apply"])
    cw.add_argument("--scheme", type=auto_int, help="0 = zero skew, 1 = mesh")
    cw.add_argument("--mode", type=auto_int, nargs="*",
                    help="CONTROL_MODE_0..3; unspecified registers keep their value")
    cw.add_argument("--skip-modes", action="store_true")
    cw.add_argument("--skip-scheme", action="store_true")

    fl = sub.add_parser("fll", help="FLL droop detector (0x24)")
    fl.add_argument("op", choices=["get", "dump", "set", "pulse", "ro-count",
                                   "sample-nominal"])
    fl.add_argument("--index", type=auto_int, default=0, help="FLL_CNTL index")
    fl.add_argument("--value", type=auto_int, default=0)
    fl.add_argument("--mask", type=auto_int, default=0,
                    help="0 means write the whole register")
    fl.add_argument("--start", type=auto_int, default=0, help="dump start index")
    fl.add_argument("--trigger", choices=sorted(TRIGGERS), nargs="+", default=["droop"])
    fl.add_argument("--cycles", type=auto_int, default=0, help="0 uses the default of 10")
    fl.add_argument("--reads", type=auto_int, default=0)
    fl.add_argument("--discard", type=auto_int, default=0)

    args = parser.parse_args()
    smc = Smc(args.interface, args.queue)
    if args.cmd == "clock-wave":
        cmd_clock_wave(smc, args)
    else:
        cmd_fll(smc, args)


if __name__ == "__main__":
    main()
