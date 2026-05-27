#!/usr/bin/env python3
"""run_on_target.py — host-side on-target serial runner for the PicoSystem.

Flashes a firmware .uf2 via picotool, reads a KEY=VALUE result stream from the
device USB-CDC tty until a sentinel, and asserts parsed values against
thresholds. Exits non-zero on any miss so CI / the C-gates can gate on it.

Design: the parse + threshold-assert logic is a PURE, unit-tested core
(parse_stream / assert_thresholds / parse_check_spec). The picotool/tty I/O is
a thin shell around that core and needs hardware to exercise; the core does
not (see run_on_target_test.py).

Usage:
    run_on_target.py <firmware.uf2> [--tty /dev/ttyACM0]
                     [--sentinel PROBE_DONE] [--timeout SECS]
                     --check 'frame_us<=16667' --check 'fps>=60' ...

picotool subcommands used (verified against `picotool help`):
  reset-to-BOOTSEL:  picotool reboot -f -u   (force a running pico into BOOTSEL)
  flash + execute:   picotool load -x <fw.uf2>
BOOTSEL fallback: if reboot fails (device hung / no compatible code to force),
the runner prints a "press BOOTSEL" prompt and waits for the drive to appear
(human in the loop) — see WORKFLOW Unresolved Q3.
"""

import argparse
import glob
import re
import subprocess
import sys
import time
from collections import namedtuple

# ---- pure core (unit-tested; no hardware) -----------------------------------

# A single threshold check: parsed[key] <op> value, op in OPS.
Check = namedtuple("Check", ["key", "op", "value"])

OPS = {
    "<=": lambda a, b: a <= b,
    ">=": lambda a, b: a >= b,
    "==": lambda a, b: a == b,
    "<": lambda a, b: a < b,
    ">": lambda a, b: a > b,
}

# Longest operators first so '<=' is matched before '<'.
_OP_ALTERNATION = "|".join(re.escape(op) for op in sorted(OPS, key=len, reverse=True))
_CHECK_RE = re.compile(r"^\s*([A-Za-z0-9_]+)\s*(" + _OP_ALTERNATION + r")\s*(-?[0-9.]+)\s*$")


def parse_stream(lines):
    """Parse an iterable of text lines into a dict of KEY=VALUE pairs.

    Lines without a '=' are ignored (start banners, noise). Key and value are
    whitespace-stripped; the value may itself contain '=' (split once). On a
    duplicate key the last value wins.
    """
    result = {}
    for line in lines:
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        if key:
            result[key] = value
    return result


def read_until_sentinel(lines, sentinel):
    """Yield lines from an iterable up to (not including) the sentinel line.

    The sentinel match is on the stripped line. If the sentinel never appears,
    every line is yielded.
    """
    for line in lines:
        if line.strip() == sentinel:
            return
        yield line


def parse_check_spec(spec):
    """Parse a 'KEY<op>VALUE' threshold spec string into a Check.

    Raises ValueError on a malformed spec.
    """
    match = _CHECK_RE.match(spec)
    if not match:
        raise ValueError("malformed check spec: %r" % spec)
    key, op, value = match.group(1), match.group(2), match.group(3)
    return Check(key, op, float(value))


def assert_thresholds(parsed, checks):
    """Compare parsed values against checks; return a list of failure strings.

    Empty list == all checks passed. A missing key or a non-numeric value is a
    failure (reported, not raised) so every check is evaluated and reported.
    """
    failures = []
    for check in checks:
        if check.key not in parsed:
            failures.append("%s: missing from device output" % check.key)
            continue
        raw = parsed[check.key]
        try:
            actual = float(raw)
        except ValueError:
            failures.append("%s: non-numeric value %r" % (check.key, raw))
            continue
        if not OPS[check.op](actual, check.value):
            failures.append(
                "%s: %g %s %g failed" % (check.key, actual, check.op, check.value)
            )
    return failures


# ---- thin device-I/O shell (needs hardware) ---------------------------------


def run_picotool(args):
    """Run `picotool <args>`; return (returncode, stdout+stderr)."""
    proc = subprocess.run(
        ["picotool"] + args,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return proc.returncode, proc.stdout


def reset_to_bootsel():
    """Force a running device into BOOTSEL mode: `picotool reboot -f -u`.

    Returns True on success. On failure the caller should fall back to the
    human BOOTSEL prompt (the device may be hung with no resettable code).
    """
    code, out = run_picotool(["reboot", "-f", "-u"])
    if code != 0:
        sys.stderr.write(out)
    return code == 0


def wait_for_bootsel_fallback(timeout):
    """Human-in-the-loop BOOTSEL fallback (Unresolved Q3).

    Prompts the operator to hold BOOTSEL and replug/reset, then waits for a
    device in BOOTSEL to appear (picotool info succeeds).
    """
    sys.stderr.write(
        "\n*** picotool could not reset the device. ***\n"
        "Hold BOOTSEL and tap reset (or replug), then release.\n"
        "Waiting for BOOTSEL device...\n"
    )
    deadline = time.time() + timeout
    while time.time() < deadline:
        code, _ = run_picotool(["info"])
        if code == 0:
            return True
        time.sleep(0.5)
    return False


def flash(firmware):
    """Load + execute firmware onto a device in BOOTSEL: `picotool load -x`."""
    code, out = run_picotool(["load", "-x", firmware])
    if code != 0:
        sys.stderr.write(out)
    return code == 0


def wait_for_tty(pattern, timeout):
    """Poll for a USB-CDC tty matching glob `pattern`; return path or None."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
        time.sleep(0.25)
    return None


def read_lines_from_tty(tty, sentinel, timeout):
    """Read newline-delimited lines from the tty until the sentinel or timeout.

    Returns the collected lines (excluding the sentinel). Uses pyserial if
    available, else a plain file read (the device appears as a character
    device; line-buffered reads work for typical CDC firmware).
    """
    deadline = time.time() + timeout
    lines = []
    try:
        import serial  # pyserial, optional

        with serial.Serial(tty, timeout=1) as port:
            while time.time() < deadline:
                raw = port.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", "replace").rstrip("\r\n")
                if line.strip() == sentinel:
                    return lines
                lines.append(line)
        return lines
    except ImportError:
        with open(tty, "r", errors="replace") as port:
            for line in port:
                if time.time() >= deadline:
                    break
                line = line.rstrip("\r\n")
                if line.strip() == sentinel:
                    return lines
                lines.append(line)
        return lines


# ---- orchestration ----------------------------------------------------------


def parse_args(argv):
    parser = argparse.ArgumentParser(description="On-target serial runner.")
    parser.add_argument("firmware", help="firmware .uf2 to flash")
    parser.add_argument("--tty", default="/dev/ttyACM*", help="tty glob (default /dev/ttyACM*)")
    parser.add_argument("--sentinel", default="PROBE_DONE", help="terminating line")
    parser.add_argument("--timeout", type=float, default=30.0, help="per-stage timeout (s)")
    parser.add_argument(
        "--check",
        action="append",
        default=[],
        metavar="KEY<op>VAL",
        help="threshold check, e.g. 'frame_us<=16667' (repeatable)",
    )
    parser.add_argument("--no-flash", action="store_true", help="skip flash; just read")
    return parser.parse_args(argv)


def main(argv):
    args = parse_args(argv)

    try:
        checks = [parse_check_spec(spec) for spec in args.check]
    except ValueError as err:
        sys.stderr.write("error: %s\n" % err)
        return 2

    if not args.no_flash:
        if not reset_to_bootsel():
            if not wait_for_bootsel_fallback(args.timeout):
                sys.stderr.write("error: no BOOTSEL device after fallback\n")
                return 3
        if not flash(args.firmware):
            sys.stderr.write("error: picotool load failed\n")
            return 3

    tty = wait_for_tty(args.tty, args.timeout)
    if tty is None:
        sys.stderr.write("error: no tty matched %r within %gs\n" % (args.tty, args.timeout))
        return 4

    raw_lines = read_lines_from_tty(tty, args.sentinel, args.timeout)
    parsed = parse_stream(raw_lines)

    sys.stdout.write("--- device output ---\n")
    for key in sorted(parsed):
        sys.stdout.write("%s=%s\n" % (key, parsed[key]))

    failures = assert_thresholds(parsed, checks)
    if failures:
        sys.stderr.write("--- threshold failures ---\n")
        for failure in failures:
            sys.stderr.write("FAIL %s\n" % failure)
        sys.stderr.write("--- raw device lines ---\n")
        for line in raw_lines:
            sys.stderr.write(line + "\n")
        return 1

    sys.stdout.write("all %d checks passed\n" % len(checks))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
