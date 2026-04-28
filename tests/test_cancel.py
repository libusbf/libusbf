# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Robert Baldyga

"""Host-side test for the cancel_loopback gadget.

Drives usbf_cancel() and usbf_cancel_all() through their full round-trip:

  1. gadget pre-submits 4 OUT buffers on enable (no auto-resubmit),
  2. host issues a vendor request that triggers the cancel call on the gadget,
  3. host reads back stats and verifies:
        - cancel_returned == number of in-flight submits (4),
        - cb_count == cancel_returned (every canceled submit fired its
          completion callback),
        - last_result < 0 (kernel reported a cancellation errno).

Both flavors of cancel are covered: per-endpoint (usbf_cancel) and whole-
function (usbf_cancel_all).
"""

import struct
import time

import pytest
import usb.core
import usb.util

VID = 0x1d6b
PID = 0x0104

INTERFACE_NUM = 0
GADGET = "gadgets/cancel_loopback"
TIMEOUT_MS = 1000

VR_CANCEL_EP  = 0x01
VR_CANCEL_ALL = 0x02
VR_GET_STATS  = 0x03

EXPECTED_INFLIGHT = 4

# struct { int32_t cancel_returned; uint16_t cb_count; int16_t last_result; }
STATS_FMT = "<ihh"
STATS_LEN = struct.calcsize(STATS_FMT)


def _open_device():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    assert dev is not None, \
        f"no device with VID:PID {VID:04x}:{PID:04x} found"
    dev.set_configuration()
    return dev


def _bulk_endpoints(dev):
    intf = dev.get_active_configuration()[(0, 0)]
    ep_in = usb.util.find_descriptor(
        intf, custom_match=lambda e:
            usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)
    ep_out = usb.util.find_descriptor(
        intf, custom_match=lambda e:
            usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
    return intf, ep_in, ep_out


def _ep_number(ep):
    return ep.bEndpointAddress & 0x7f


def _read_stats(dev):
    raw = dev.ctrl_transfer(0xC1, VR_GET_STATS, 0, INTERFACE_NUM,
                            STATS_LEN, TIMEOUT_MS)
    assert len(raw) == STATS_LEN, \
        f"expected {STATS_LEN}-byte stats, got {len(raw)}"
    cancel_returned, cb_count, last_result = struct.unpack(STATS_FMT, bytes(raw))
    return cancel_returned, cb_count, last_result


def _wait_for_callbacks(dev, expected, deadline_s=2.0):
    """Poll the stats endpoint until cb_count reaches `expected`.

    FFS may defer AIO completion via a workqueue, so cancel callbacks are
    not guaranteed to have fired by the time the cancel ACK lands at the
    host. We poll briefly to absorb that delay without imposing a fixed
    sleep on every run.
    """
    deadline = time.monotonic() + deadline_s
    while True:
        cancel_returned, cb_count, last_result = _read_stats(dev)
        if cb_count >= expected:
            return cancel_returned, cb_count, last_result
        if time.monotonic() >= deadline:
            pytest.fail(
                f"timeout waiting for {expected} callbacks; "
                f"got cb_count={cb_count}, "
                f"cancel_returned={cancel_returned}, "
                f"last_result={last_result}")
        time.sleep(0.01)


@pytest.mark.parametrize("gadget", [GADGET], indirect=True)
def test_cancel_endpoint(gadget):
    """usbf_cancel(ep) cancels every in-flight submit on that endpoint."""
    dev = _open_device()
    _, _, ep_out = _bulk_endpoints(dev)
    usb.util.claim_interface(dev, INTERFACE_NUM)
    try:
        dev.ctrl_transfer(0x41, VR_CANCEL_EP, _ep_number(ep_out),
                          INTERFACE_NUM, b"", TIMEOUT_MS)

        cancel_returned, cb_count, last_result = \
            _wait_for_callbacks(dev, EXPECTED_INFLIGHT)

        assert cancel_returned == EXPECTED_INFLIGHT, \
            f"usbf_cancel returned {cancel_returned}, expected {EXPECTED_INFLIGHT}"
        assert cb_count == EXPECTED_INFLIGHT, \
            f"{cb_count} callbacks fired, expected {EXPECTED_INFLIGHT}"
        assert last_result < 0, \
            f"expected negative cancellation result, got {last_result}"
    finally:
        usb.util.release_interface(dev, INTERFACE_NUM)
        usb.util.dispose_resources(dev)


@pytest.mark.parametrize("gadget", [GADGET], indirect=True)
def test_cancel_all(gadget):
    """usbf_cancel_all(func) cancels every in-flight submit on the function."""
    dev = _open_device()
    usb.util.claim_interface(dev, INTERFACE_NUM)
    try:
        dev.ctrl_transfer(0x41, VR_CANCEL_ALL, 0, INTERFACE_NUM,
                          b"", TIMEOUT_MS)

        cancel_returned, cb_count, last_result = \
            _wait_for_callbacks(dev, EXPECTED_INFLIGHT)

        assert cancel_returned == EXPECTED_INFLIGHT, \
            f"usbf_cancel_all returned {cancel_returned}, expected {EXPECTED_INFLIGHT}"
        assert cb_count == EXPECTED_INFLIGHT, \
            f"{cb_count} callbacks fired, expected {EXPECTED_INFLIGHT}"
        assert last_result < 0, \
            f"expected negative cancellation result, got {last_result}"
    finally:
        usb.util.release_interface(dev, INTERFACE_NUM)
        usb.util.dispose_resources(dev)
