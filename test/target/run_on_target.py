#!/usr/bin/env python3
"""Runs the on-target test firmware and reports the result.

Talks to the runner in test/target/<platform>/main.cpp: waits for
CORO_TEST_READY, sends RUN with an optional gtest filter, echoes the output, and
stops at CORO_TEST_END. See doc/design/on_target_tests.md.

    run_on_target.py build/coro_target_*.uf2          # flash and run each in turn
    run_on_target.py                                  # board already flashed
    run_on_target.py --filter 'OneshotTest/*'
    run_on_target.py --emulator build/coro_target_*.uf2   # no board: emulate it

The suite is split across several firmware images because one does not hold it
all. Given images, each is loaded and run, and the results are added up.

With --emulator each image runs in an emulated RP2040 (test/target/emulator)
instead of on a board. That is a quick check for a machine with no board
attached, such as a CI runner. It does not replace a run on the hardware.

Exit status: 0 all tests passed, 1 a test failed, 2 a run did not finish (the
device hung, crashed, or never answered). With several images, the worst of them.

On a board: needs pyserial, and picotool on PATH to load an image.
With --emulator: needs Node.js 18 or later (node and npm on PATH). The first run
installs the emulator with npm and downloads the RP2040 boot ROM, both into
test/target/emulator. --work-dir puts them somewhere else, for a source tree that
cannot be written to.
"""

import argparse
import hashlib
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.request

RASPBERRY_PI_USB_VENDOR_ID = 0x2E8A

EMULATOR_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "emulator")
# What --work-dir copies out of EMULATOR_DIR. The runner goes with the package files
# because Node looks for an ES module's packages only in a node_modules above the
# module itself.
EMULATOR_FILES = ("package.json", "package-lock.json", "run_rp2040.mjs")
# The emulator has no boot ROM of its own and the Pico SDK calls into it. B1 is
# the revision the emulator's own tests use.
BOOTROM_URL = ("https://github.com/raspberrypi/pico-bootrom-rp2040/"
               "releases/download/b1/b1.elf")
BOOTROM_SHA256 = "53b2723b590207c678c5a52c095e9150d2afa60e7f66a1c26d188ff59d29c778"
BOOTROM_FILE = os.path.join(".cache", "b1.elf")   # inside the emulator's directory

RUN_LINE = re.compile(r"^\[ RUN      \] (\S+)")
OK_LINE = re.compile(r"^\[       OK \] (\S+)")
FAILED_LINE = re.compile(r"^\[  FAILED  \] (\S+?)(?:,| \(|$)")
END_LINE = re.compile(r"^CORO_TEST_END rc=(-?\d+) heap_used=(\d+) heap_peak=(\d+)")
HEAP_LINE = re.compile(r"^CORO_TEST_HEAP used=(\d+)")
BEGIN_LINE = re.compile(r"^CORO_TEST_BEGIN heap_total=(\d+) heap_used=(\d+)")


class LinkLost(Exception):
    """The device went away mid-run: the serial port vanished, or the emulator
    exited."""


class SerialLink:
    """A board's serial port."""

    def __init__(self, port):
        import serial   # pyserial; not needed with --emulator
        self._serial_error = serial.SerialException
        self._port = serial.Serial(port, 115200, timeout=0.5)

    def readline(self):
        """One line, or b"" if none arrived within half a second."""
        try:
            return self._port.readline()
        except self._serial_error as e:
            # The port vanishing mid-run is what a crash or reset looks like
            # over USB.
            raise LinkLost(f"lost the serial port: {e}") from e

    def write(self, data):
        self._port.write(data)
        self._port.flush()

    def close(self):
        self._port.close()


class EmulatorLink:
    """An emulated RP2040 running one image, its UART on the process's pipes."""

    def __init__(self, image, emulator_dir):
        command = ["node", os.path.join(emulator_dir, "run_rp2040.mjs"),
                   "--bootrom", os.path.join(emulator_dir, BOOTROM_FILE), image]
        # stderr is inherited: what the emulator itself reports goes straight
        # to the terminal.
        self._process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE)
        # A pipe has no read timeout, so a thread reads it and hands the lines
        # over. None marks the end of the output.
        self._lines = queue.Queue()
        self._reader = threading.Thread(target=self._read_lines, daemon=True)
        self._reader.start()

    def _read_lines(self):
        for line in self._process.stdout:
            self._lines.put(line)
        self._lines.put(None)

    def readline(self):
        """One line, or b"" if none arrived within half a second."""
        try:
            line = self._lines.get(timeout=0.5)
        except queue.Empty:
            return b""
        if line is None:
            self._lines.put(None)   # stays lost for any later call
            raise LinkLost(f"the emulator exited with status {self._process.wait()}")
        return line

    def write(self, data):
        try:
            self._process.stdin.write(data)
            self._process.stdin.flush()
        except BrokenPipeError as e:
            raise LinkLost("the emulator exited") from e

    def close(self):
        self._process.kill()
        self._process.wait()


def find_port():
    from serial.tools import list_ports
    ports = [p.device for p in list_ports.comports()
             if p.vid == RASPBERRY_PI_USB_VENDOR_ID]
    return ports[0] if len(ports) == 1 else None


def wait_for_port(explicit, timeout):
    """Returns the port to use, waiting for the board to enumerate."""
    from serial.tools import list_ports
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if explicit:
            if any(p.device == explicit for p in list_ports.comports()):
                return explicit
        else:
            port = find_port()
            if port:
                return port
        time.sleep(0.2)
    return None


def flash(image):
    # -f reboots a board that is running firmware with USB stdio into the
    # bootloader first; -x starts the new image afterwards.
    subprocess.run(["picotool", "load", "-f", "-x", image], check=True)


def read_file(path):
    """The file's contents, or None if there is no such file."""
    try:
        with open(path, "rb") as f:
            return f.read()
    except FileNotFoundError:
        return None


def prepare_emulator(emulator_dir):
    """Installs the emulator in `emulator_dir` and fetches the boot ROM, the first
    time. Returns an error message, or None when the emulator is ready."""
    if shutil.which("node") is None or shutil.which("npm") is None:
        return "--emulator needs Node.js: node and npm were not found on PATH"

    install = not os.path.isdir(os.path.join(emulator_dir, "node_modules", "rp2040js"))
    if emulator_dir != EMULATOR_DIR:
        # --work-dir: the emulator's own files are copied in on every run, so the
        # work directory never runs an old runner. A changed package-lock.json
        # means what is installed there is out of date.
        lock = "package-lock.json"
        if (read_file(os.path.join(emulator_dir, lock))
                != read_file(os.path.join(EMULATOR_DIR, lock))):
            install = True
        try:
            os.makedirs(emulator_dir, exist_ok=True)
            for name in EMULATOR_FILES:
                shutil.copyfile(os.path.join(EMULATOR_DIR, name),
                                os.path.join(emulator_dir, name))
        except OSError as e:
            return f"could not set up the work directory {emulator_dir}: {e}"

    if install:
        print(f"installing the emulator in {emulator_dir}", file=sys.stderr)
        # npm ci installs exactly what package-lock.json names.
        installed = subprocess.run(["npm", "ci", "--ignore-scripts", "--no-audit",
                                    "--no-fund"], cwd=emulator_dir)
        if installed.returncode != 0:
            return "npm ci failed"

    bootrom_path = os.path.join(emulator_dir, BOOTROM_FILE)
    if not os.path.isfile(bootrom_path):
        print(f"downloading the RP2040 boot ROM from {BOOTROM_URL}", file=sys.stderr)
        try:
            with urllib.request.urlopen(BOOTROM_URL, timeout=60) as response:
                data = response.read()
        except OSError as e:
            return f"could not download the boot ROM: {e}"
        if hashlib.sha256(data).hexdigest() != BOOTROM_SHA256:
            return "the downloaded boot ROM does not have the expected checksum"
        os.makedirs(os.path.dirname(bootrom_path), exist_ok=True)
        with open(bootrom_path, "wb") as f:
            f.write(data)
    return None


def run_on_board(args):
    port = wait_for_port(args.port, args.connect_timeout)
    if port is None:
        print("error: no serial port found (pass --port, or check that exactly "
              "one Pico is connected)", file=sys.stderr)
        return 2
    link = SerialLink(port)
    try:
        return run(args, link)
    finally:
        link.close()


def run_in_emulator(args, image, emulator_dir):
    link = EmulatorLink(image, emulator_dir)
    try:
        return run(args, link)
    finally:
        link.close()


def run(args, link):
    """Runs the tests over an open link and returns the exit status."""
    passed, failed = [], []
    running = None       # test named by the last "[ RUN ]" with no result yet
    started = False
    heap_total = heap_at_start = None
    heap_before_test = None   # from the CORO_TEST_HEAP line before `running`

    run_started = time.monotonic()
    overall_deadline = run_started + args.timeout
    last_output = run_started

    while True:
        now = time.monotonic()
        if now > overall_deadline:
            print(f"\nerror: no result after {args.timeout}s", file=sys.stderr)
            break
        if now - last_output > args.idle_timeout:
            print(f"\nerror: no output for {args.idle_timeout}s",
                  file=sys.stderr)
            break

        try:
            raw = link.readline()
            if not raw:
                continue
            last_output = time.monotonic()
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")

            if not started:
                if line == "CORO_TEST_READY":
                    link.write(f"RUN {args.filter}\n".encode())
                elif (m := BEGIN_LINE.match(line)):
                    started = True
                    heap_total, heap_at_start = int(m[1]), int(m[2])
                    print(line, flush=True)
                continue
        except LinkLost as e:
            print(f"\nerror: {e}", file=sys.stderr)
            break

        if (m := HEAP_LINE.match(line)):
            heap_before_test = int(m[1])
            continue
        print(line, flush=True)
        if (m := RUN_LINE.match(line)):
            running = m[1]
        elif (m := OK_LINE.match(line)):
            passed.append(m[1])
            running = None
        elif (m := FAILED_LINE.match(line)) and running == m[1]:
            # Only the per-test line: gtest lists the failed tests again in
            # its summary, by which time `running` is None.
            failed.append(m[1])
            running = None
        elif (m := END_LINE.match(line)):
            rc, heap_used, heap_peak = int(m[1]), int(m[2]), int(m[3])
            print()
            # Time on this machine, from opening the link. The times gtest prints
            # are the device's own, which in the emulator is not the same thing.
            print(f"passed: {len(passed)}   failed: {len(failed)}   "
                  f"elapsed: {time.monotonic() - run_started:.1f}s")
            print(f"heap: {heap_total} bytes total, {heap_at_start} used "
                  f"before the first test, {heap_peak} at peak, "
                  f"{heap_used} still allocated at the end")
            for name in failed:
                print(f"  FAILED {name}")
            return 0 if rc == 0 and not failed else 1

    # Reached only when the run did not finish.
    if not started:
        print("the device never announced CORO_TEST_READY / CORO_TEST_BEGIN",
              file=sys.stderr)
    elif running:
        print(f"the device stopped during: {running}", file=sys.stderr)
        if heap_before_test is not None and heap_total:
            print(f"heap when that test started: {heap_before_test} of "
                  f"{heap_total} bytes in use, {heap_total - heap_before_test} free",
                  file=sys.stderr)
    print(f"passed before stopping: {len(passed)}   failed: {len(failed)}",
          file=sys.stderr)
    return 2


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", help="serial port (default: the only Pico found)")
    parser.add_argument("images", nargs="*", metavar="IMAGE",
                        help=".uf2 or .elf to load with picotool and run; "
                             "with none, runs what is on the board")
    parser.add_argument("--emulator", action="store_true",
                        help="run each IMAGE (.uf2) in an emulated RP2040 "
                             "instead of on a board")
    parser.add_argument("--work-dir", metavar="DIR",
                        help="with --emulator: install the emulator and keep the "
                             "boot ROM in DIR instead of test/target/emulator, "
                             "for a source tree that cannot be written to")
    parser.add_argument("--filter", default="*", help="gtest filter (default: *)")
    parser.add_argument("--timeout", type=float, default=300,
                        help="seconds allowed for each image's run (default: 300)")
    parser.add_argument("--idle-timeout", type=float, default=30,
                        help="seconds without output before giving up (default: 30)")
    parser.add_argument("--connect-timeout", type=float, default=15,
                        help="seconds to wait for the serial port (default: 15)")
    args = parser.parse_args()

    if args.work_dir and not args.emulator:
        parser.error("--work-dir applies only with --emulator")

    emulator_dir = os.path.abspath(args.work_dir) if args.work_dir else EMULATOR_DIR
    if args.emulator:
        if not args.images:
            parser.error("--emulator needs at least one IMAGE")
        if args.port:
            parser.error("--port does not apply with --emulator")
        for image in args.images:
            if not image.endswith(".uf2"):
                parser.error(f"--emulator loads .uf2 images: {image}")
        if (problem := prepare_emulator(emulator_dir)):
            print(f"error: {problem}", file=sys.stderr)
            return 2
    elif not args.images:
        return run_on_board(args)

    results = []
    for image in args.images:
        print(f"\n===== {image} =====", flush=True)
        if args.emulator:
            if not os.path.isfile(image):
                print(f"error: no such file: {image}", file=sys.stderr)
                results.append((image, 2))
                continue
            results.append((image, run_in_emulator(args, image, emulator_dir)))
            continue
        try:
            flash(image)
        except (subprocess.CalledProcessError, FileNotFoundError) as e:
            print(f"error: could not load {image}: {e}", file=sys.stderr)
            results.append((image, 2))
            continue
        results.append((image, run_on_board(args)))

    if len(results) > 1:
        print("\n===== all images =====")
        words = {0: "passed", 1: "FAILED", 2: "DID NOT FINISH"}
        for image, status in results:
            print(f"  {words[status]:<15}{image}")
    return max(status for _, status in results)


if __name__ == "__main__":
    sys.exit(main())
