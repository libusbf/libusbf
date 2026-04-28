# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Robert Baldyga

"""Host-side test for the control_loopback gadget.

The gadget exposes one vendor-specific interface with no data endpoints; all
I/O is on ep0.  Vendor request 0x01:

  bmRequestType=0x41 (vendor, OUT, interface): host writes data; gadget stores it.
  bmRequestType=0xC1 (vendor, IN,  interface): gadget returns previously stored
                                               bytes (truncated to wLength).
"""

import secrets

import pytest
import usb.core
import usb.util

VID = 0x1d6b
PID = 0x0104

EXPECTED_INTERFACE_CLASS = 0xff
EXPECTED_INTERFACE_STRING = "control_loopback"
EXPECTED_NUM_ENDPOINTS = 0
EXPECTED_NUM_ALTSETTINGS = 1

GADGET = "gadgets/control_loopback"

VENDOR_OUT_IFACE = 0x41
VENDOR_IN_IFACE = 0xc1
LOOPBACK_REQUEST = 0x01

SIZES = [1, 16, 64, 256, 512]
ITERATIONS = 5
TIMEOUT_MS = 1000


def _validate_configuration(dev):
    assert dev.bNumConfigurations == 1, \
        f"expected 1 configuration, got {dev.bNumConfigurations}"

    cfg = dev.get_active_configuration()
    assert cfg.bNumInterfaces == 1, \
        f"expected 1 interface, got {cfg.bNumInterfaces}"

    alts = [i for i in cfg if i.bInterfaceNumber == 0]
    assert len(alts) == EXPECTED_NUM_ALTSETTINGS, (
        f"expected {EXPECTED_NUM_ALTSETTINGS} alt setting(s) on interface 0, "
        f"got {len(alts)}: {[i.bAlternateSetting for i in alts]}")

    intf = alts[0]
    assert intf.bAlternateSetting == 0, \
        f"expected bAlternateSetting=0, got {intf.bAlternateSetting}"
    assert intf.bInterfaceClass == EXPECTED_INTERFACE_CLASS, (
        f"expected bInterfaceClass=0x{EXPECTED_INTERFACE_CLASS:02x}, "
        f"got 0x{intf.bInterfaceClass:02x}")
    assert intf.bNumEndpoints == EXPECTED_NUM_ENDPOINTS, \
        f"expected {EXPECTED_NUM_ENDPOINTS} endpoints, got {intf.bNumEndpoints}"

    iface_str = usb.util.get_string(dev, intf.iInterface) if intf.iInterface else None
    assert iface_str == EXPECTED_INTERFACE_STRING, (
        f"expected iInterface string {EXPECTED_INTERFACE_STRING!r}, "
        f"got {iface_str!r}")
    return intf


def _open_device():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    assert dev is not None, \
        f"no device with VID:PID {VID:04x}:{PID:04x} found"
    dev.set_configuration()
    return dev


def _ctrl_loopback(dev, iface, length):
    payload = secrets.token_bytes(length)
    dev.ctrl_transfer(VENDOR_OUT_IFACE, LOOPBACK_REQUEST, 0, iface,
                      payload, TIMEOUT_MS)
    received = bytes(dev.ctrl_transfer(VENDOR_IN_IFACE, LOOPBACK_REQUEST,
                                       0, iface, length, TIMEOUT_MS))
    assert received == payload, \
        f"control loopback mismatch (sent {length} B, got {len(received)} B)"


@pytest.mark.parametrize("gadget", [GADGET], indirect=True)
def test_descriptors(gadget):
    dev = _open_device()
    try:
        _validate_configuration(dev)
    finally:
        usb.util.dispose_resources(dev)


@pytest.mark.parametrize("gadget", [GADGET], indirect=True)
def test_loopback(gadget):
    dev = _open_device()
    try:
        for size in SIZES:
            for _ in range(ITERATIONS):
                _ctrl_loopback(dev, 0, size)
    finally:
        usb.util.dispose_resources(dev)
