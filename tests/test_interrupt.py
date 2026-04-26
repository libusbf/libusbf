# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Robert Baldyga

"""Host-side test for the interrupt_loopback gadget.

Same shape as bulk_loopback but with two interrupt endpoints (64-byte mps).
"""

import secrets

import pytest
import usb.core
import usb.util

VID = 0x1d6b
PID = 0x0104

EXPECTED_INTERFACE_CLASS = 0xff
EXPECTED_INTERFACE_STRING = "interrupt_loopback"
EXPECTED_NUM_ENDPOINTS = 2
EXPECTED_NUM_ALTSETTINGS = 1

EXPECTED_MAXPACKET = 64

GADGET = "gadgets/interrupt_loopback"
SIZES = [1, 32, 64, 128, 512]
ITERATIONS = 5
TIMEOUT_MS = 2000


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

    ep_in = ep_out = None
    for ep in intf:
        attr = ep.bmAttributes & 0x03
        assert attr == 0x03, (
            f"endpoint 0x{ep.bEndpointAddress:02x} is not interrupt "
            f"(bmAttributes=0x{ep.bmAttributes:02x})")
        assert ep.wMaxPacketSize == EXPECTED_MAXPACKET, (
            f"endpoint 0x{ep.bEndpointAddress:02x} has unexpected "
            f"wMaxPacketSize={ep.wMaxPacketSize} (expected {EXPECTED_MAXPACKET})")
        if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_IN:
            assert ep_in is None, "more than one interrupt IN endpoint"
            ep_in = ep
        else:
            assert ep_out is None, "more than one interrupt OUT endpoint"
            ep_out = ep

    assert ep_in is not None, "no interrupt IN endpoint"
    assert ep_out is not None, "no interrupt OUT endpoint"
    return intf, ep_in, ep_out


def _open_device():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    assert dev is not None, \
        f"no device with VID:PID {VID:04x}:{PID:04x} found"
    dev.set_configuration()
    return dev


def _interrupt_endpoints(dev):
    intf = dev.get_active_configuration()[(0, 0)]
    ep_in = usb.util.find_descriptor(
        intf, custom_match=lambda e:
            usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)
    ep_out = usb.util.find_descriptor(
        intf, custom_match=lambda e:
            usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
    return intf, ep_in, ep_out


def _roundtrip(ep_in, ep_out, length, timeout_ms):
    payload = secrets.token_bytes(length)
    chunk = ep_out.wMaxPacketSize
    received = bytearray()
    for off in range(0, length, chunk):
        slice_ = payload[off:off + chunk]
        written = ep_out.write(slice_, timeout=timeout_ms)
        assert written == len(slice_), \
            f"short write at offset {off}: {written}/{len(slice_)}"
        received += bytes(ep_in.read(len(slice_), timeout=timeout_ms))
    assert bytes(received) == payload, \
        f"loopback mismatch (sent {length} B, got {len(received)} B)"


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
    intf, ep_in, ep_out = _interrupt_endpoints(dev)
    usb.util.claim_interface(dev, intf.bInterfaceNumber)
    try:
        for size in SIZES:
            for _ in range(ITERATIONS):
                _roundtrip(ep_in, ep_out, size, TIMEOUT_MS)
    finally:
        usb.util.release_interface(dev, intf.bInterfaceNumber)
        usb.util.dispose_resources(dev)
