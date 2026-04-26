# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Robert Baldyga

"""Host-side test for the hid_minimal gadget.

Validates that libusbf emits a class-specific descriptor blob (the HID class
descriptor) between the interface descriptor and its endpoint descriptors,
and that the resulting configuration descriptor parses cleanly on the host.
The gadget is not a functional HID device (no report descriptor is served);
the test only inspects descriptors, not data flow.
"""

import struct

import pytest
import usb.core
import usb.util

VID = 0x1d6b
PID = 0x0104

INTERFACE_NUM = 0
EXPECTED_INTERFACE_CLASS = 0x03   # HID
EXPECTED_INTERFACE_STRING = "hid_minimal"
EXPECTED_HID_DESC_LEN = 9
EXPECTED_HID_DESC_TYPE = 0x21
EXPECTED_BCD_HID = 0x0111
EXPECTED_NUM_REPORT_DESCS = 1
EXPECTED_REPORT_DESC_TYPE = 0x22
EXPECTED_REPORT_DESC_LEN = 22

GADGET = "gadgets/hid_minimal"


def _open_device():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    assert dev is not None, \
        f"no device with VID:PID {VID:04x}:{PID:04x} found"
    dev.set_configuration()
    return dev


@pytest.mark.parametrize("gadget", [GADGET], indirect=True)
def test_descriptors(gadget):
    dev = _open_device()
    try:
        cfg = dev.get_active_configuration()
        assert cfg.bNumInterfaces == 1
        intf = cfg[(INTERFACE_NUM, 0)]

        assert intf.bInterfaceClass == EXPECTED_INTERFACE_CLASS, (
            f"bInterfaceClass=0x{intf.bInterfaceClass:02x} "
            f"(expected 0x{EXPECTED_INTERFACE_CLASS:02x})")
        assert intf.bInterfaceSubClass == 0
        assert intf.bInterfaceProtocol == 0
        assert intf.bNumEndpoints == 1

        s = usb.util.get_string(dev, intf.iInterface) if intf.iInterface else None
        assert s == EXPECTED_INTERFACE_STRING, \
            f"iInterface={s!r} (expected {EXPECTED_INTERFACE_STRING!r})"

        # PyUSB stores class-specific descriptors that follow the interface
        # descriptor (and precede the endpoint descriptors) in extra_descriptors.
        extra = bytes(intf.extra_descriptors)
        assert len(extra) == EXPECTED_HID_DESC_LEN, (
            f"extra_descriptors len={len(extra)} "
            f"(expected {EXPECTED_HID_DESC_LEN}): {extra.hex()}")
        assert extra[0] == EXPECTED_HID_DESC_LEN, \
            f"HID bLength=0x{extra[0]:02x}"
        assert extra[1] == EXPECTED_HID_DESC_TYPE, \
            f"HID bDescriptorType=0x{extra[1]:02x}"

        bcd_hid = struct.unpack_from("<H", extra, 2)[0]
        assert bcd_hid == EXPECTED_BCD_HID, f"bcdHID=0x{bcd_hid:04x}"
        assert extra[4] == 0, f"bCountryCode={extra[4]}"
        assert extra[5] == EXPECTED_NUM_REPORT_DESCS, \
            f"bNumDescriptors={extra[5]}"
        assert extra[6] == EXPECTED_REPORT_DESC_TYPE, \
            f"report bDescriptorType=0x{extra[6]:02x}"
        rpt_len = struct.unpack_from("<H", extra, 7)[0]
        assert rpt_len == EXPECTED_REPORT_DESC_LEN, f"wReportLength={rpt_len}"
    finally:
        usb.util.dispose_resources(dev)
