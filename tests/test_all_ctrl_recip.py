# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Robert Baldyga

"""Host-side test for the all_ctrl_recip gadget.

Verifies USBF_ALL_CTRL_RECIP: the gadget receives vendor/Device-recipient
setup requests that FunctionFS would otherwise refuse with -EOPNOTSUPP.
The gadget runs a tiny vendor loopback through ep0 with bRequestType
recipient = Device, so a successful round-trip proves the flag is
propagated to the FFS descriptor header and the kernel forwards the
request to setup_handler.
"""

import secrets

import pytest
import usb.core
import usb.util

VID = 0x1d6b
PID = 0x0104

GADGET = "gadgets/all_ctrl_recip"
LOOPBACK_REQUEST = 0x01
TIMEOUT_MS = 1000


def _open_device():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    assert dev is not None, \
        f"no device with VID:PID {VID:04x}:{PID:04x} found"
    dev.set_configuration()
    return dev


def _roundtrip(dev, length):
    payload = secrets.token_bytes(length)
    written = dev.ctrl_transfer(0x40, LOOPBACK_REQUEST, 0, 0,
                                payload, TIMEOUT_MS)
    assert written == length, f"OUT short write: {written}/{length}"
    received = bytes(dev.ctrl_transfer(0xC0, LOOPBACK_REQUEST, 0, 0,
                                       length, TIMEOUT_MS))
    assert received == payload, \
        f"loopback mismatch (sent {length} B, got {len(received)} B)"


@pytest.mark.parametrize("gadget", [GADGET], indirect=True)
def test_device_recipient_loopback(gadget):
    dev = _open_device()
    try:
        for size in (1, 16, 64, 256):
            _roundtrip(dev, size)
    finally:
        usb.util.dispose_resources(dev)
