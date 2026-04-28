# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Robert Baldyga

"""Pytest fixtures for libusbf gadget tests.

The `gadget` fixture spawns ``setup.sh <binary>`` as a subprocess, blocks until
it logs that the UDC is bound, and SIGINTs it on teardown so the script's own
trap-based cleanup runs.  Each test gets a fresh setup/teardown cycle -
isolation costs ~1s but means a hung or crashed gadget can't poison the next
test.
"""

import os
import pathlib
import signal
import subprocess
import time

import pytest
import usb.core

TESTS_DIR = pathlib.Path(__file__).parent
SETUP = TESTS_DIR / "setup.sh"
TEARDOWN = TESTS_DIR / "teardown.sh"

# VID/PID baked into setup.sh.  Used to confirm the device has actually
# enumerated on the host side before yielding to the test.
DEFAULT_VID = 0x1d6b
DEFAULT_PID = 0x0104

# UDC sysfs path mirrors the UDC name in setup.sh.  Used to query negotiated
# bus speed for tests that gate on dummy_hcd's mode.
DEFAULT_UDC_SYSFS = pathlib.Path("/sys/class/udc/dummy_udc.0")

READY_TIMEOUT_S = 10
STOP_TIMEOUT_S = 10

# Module parameters for dummy_hcd to advertise a given UDC speed. The kernel
# default (no params) is high-speed; the others need explicit knobs.
DUMMY_HCD_SPEED_ARGS = {
    "full-speed":  ["is_high_speed=0"],
    "high-speed":  [],
    "super-speed": ["is_super_speed=1"],
}
UDC_APPEAR_TIMEOUT_S = 5


def udc_max_speed() -> str:
    """Return the UDC's maximum_speed sysfs attribute.

    Values come from the kernel's USB speed table: 'low-speed', 'full-speed',
    'high-speed', 'super-speed', 'super-speed-plus', or 'UNKNOWN'. Tests that
    require a particular speed call this and skip if the running UDC doesn't
    advertise it. Returns 'UNKNOWN' when the sysfs entry is missing.
    """
    p = DEFAULT_UDC_SYSFS / "maximum_speed"
    try:
        return p.read_text().strip()
    except OSError:
        return "UNKNOWN"


def _wait_for_ready(log_path: pathlib.Path, proc: subprocess.Popen) -> None:
    """Block until the gadget is enumerable on the host bus.

    Two stages: setup.sh logs "gadget bound" once it writes the UDC sysfs
    entry, then the host-controller side of dummy_hcd needs a moment to
    enumerate.  We treat ``usb.core.find`` returning a device as the real
    ready signal.
    """
    deadline = time.monotonic() + READY_TIMEOUT_S
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(
                f"setup.sh exited prematurely (code {proc.returncode}):\n"
                f"{log_path.read_text()}")
        if usb.core.find(idVendor=DEFAULT_VID, idProduct=DEFAULT_PID) is not None:
            return
        time.sleep(0.1)
    proc.terminate()
    raise RuntimeError(
        f"timed out waiting for device {DEFAULT_VID:04x}:{DEFAULT_PID:04x} "
        f"to enumerate:\n{log_path.read_text()}")


@pytest.fixture
def dummy_hcd_speed(request):
    """Reload dummy_hcd to advertise a UDC at the requested speed.

    Parametrize indirectly to drive a test across speeds:

        @pytest.mark.parametrize("dummy_hcd_speed",
            ["full-speed", "high-speed", "super-speed"], indirect=True)
        @pytest.mark.parametrize("gadget", [GADGET], indirect=True)
        def test_foo(dummy_hcd_speed, gadget):
            ...

    Must be listed before `gadget` in the test signature: the gadget fixture
    binds the UDC, so the module reload has to complete first. Restores the
    default high-speed module on teardown so the rest of the suite is
    unaffected.
    """
    desired = request.param
    if desired not in DUMMY_HCD_SPEED_ARGS:
        pytest.fail(
            f"unknown dummy_hcd_speed {desired!r} (valid: "
            f"{sorted(DUMMY_HCD_SPEED_ARGS)})")

    subprocess.run(["rmmod", "dummy_hcd"], check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["modprobe", "dummy_hcd"] + DUMMY_HCD_SPEED_ARGS[desired],
                   check=True)

    deadline = time.monotonic() + UDC_APPEAR_TIMEOUT_S
    while not DEFAULT_UDC_SYSFS.exists():
        if time.monotonic() >= deadline:
            pytest.fail(
                f"dummy_udc.0 didn't appear after modprobe dummy_hcd "
                f"{' '.join(DUMMY_HCD_SPEED_ARGS[desired])}")
        time.sleep(0.05)

    try:
        yield desired
    finally:
        subprocess.run(["rmmod", "dummy_hcd"], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["modprobe", "dummy_hcd"], check=False)


@pytest.fixture
def gadget(request, tmp_path):
    """Bring up a FFS gadget for one test.

    Parametrize indirectly with the gadget binary path relative to tests/:

        @pytest.mark.parametrize("gadget", ["gadgets/basic_bulk"], indirect=True)
        def test_foo(gadget):
            ...
    """
    binary = TESTS_DIR / request.param
    if not binary.exists():
        pytest.skip(f"gadget binary not built: {binary}")

    # Defensive: previous run may have left configfs/FFS state behind.
    subprocess.run([str(TEARDOWN)], check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    log_path = tmp_path / "setup.log"
    with log_path.open("w") as log:
        proc = subprocess.Popen(
            [str(SETUP), str(binary)],
            stdout=log, stderr=subprocess.STDOUT,
            # Own process group so SIGINT goes to setup.sh and the gadget child.
            start_new_session=True,
        )

    try:
        _wait_for_ready(log_path, proc)
        yield
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGINT)
            try:
                proc.wait(timeout=STOP_TIMEOUT_S)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
