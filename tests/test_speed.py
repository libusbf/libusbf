# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Robert Baldyga

"""Host-side test for the speed_query gadget.

Parametrizes over the three speeds dummy_hcd can simulate, reloading the
module before each iteration so the gadget enumerates at exactly that
speed. The gadget calls usbf_get_speed() in its setup_handler and returns
the result over a vendor IN request; we assert it matches the speed the
module was loaded with.
"""

import pytest
import usb.core
import usb.util

VID = 0x1d6b
PID = 0x0104

GADGET = "gadgets/speed_query"
SPEED_REQUEST = 0x01
INTERFACE_NUM = 0
TIMEOUT_MS = 1000

USBF_SPEED_FS = 0x01
USBF_SPEED_HS = 0x02
USBF_SPEED_SS = 0x04

SPEED_MAP = {
    "full-speed":  USBF_SPEED_FS,
    "high-speed":  USBF_SPEED_HS,
    "super-speed": USBF_SPEED_SS,
}


def _open_device():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    assert dev is not None, \
        f"no device with VID:PID {VID:04x}:{PID:04x} found"
    dev.set_configuration()
    return dev


@pytest.mark.parametrize("dummy_hcd_speed",
                         ["full-speed", "high-speed", "super-speed"],
                         indirect=True)
@pytest.mark.parametrize("gadget", [GADGET], indirect=True)
def test_speed_matches_udc(dummy_hcd_speed, gadget):
    expected = SPEED_MAP[dummy_hcd_speed]
    dev = _open_device()
    try:
        raw = dev.ctrl_transfer(0xC1, SPEED_REQUEST, 0, INTERFACE_NUM,
                                1, TIMEOUT_MS)
        assert len(raw) == 1, f"expected 1-byte reply, got {bytes(raw)!r}"
        gadget_speed = raw[0]
        assert gadget_speed == expected, (
            f"usbf_get_speed reported 0x{gadget_speed:02x}, expected "
            f"0x{expected:02x} (dummy_hcd={dummy_hcd_speed!r})")
    finally:
        usb.util.dispose_resources(dev)
