# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Robert Baldyga

"""Host-side test for the ss_bulk_loopback gadget.

SuperSpeed-only test. The gadget declares USBF_SPEED_SS and a default SS
endpoint companion descriptor (no burst, no streams, wBytesPerInterval = 0
as required for bulk endpoints). Reloads dummy_hcd with is_super_speed=1
via the dummy_hcd_speed fixture so the test runs without requiring the
operator to flip the module out-of-band.

Two test cases: descriptor validation (verifies the SS endpoint companion
appears after each endpoint and carries the expected bytes) and a bulk
loopback at SS data rates.
"""

import secrets

import pytest
import usb.core
import usb.util

VID = 0x1d6b
PID = 0x0104

EXPECTED_INTERFACE_CLASS = 0xff
EXPECTED_INTERFACE_STRING = "ss_bulk_loopback"
EXPECTED_NUM_ENDPOINTS = 2
EXPECTED_SS_MAXPACKET = 1024

# SS endpoint companion descriptor layout: bLength=6, bDescriptorType=0x30,
# bMaxBurst, bmAttributes, wBytesPerInterval (LE).
SS_COMP_LEN = 6
SS_COMP_TYPE = 0x30
EXPECTED_BMAXBURST = 0
EXPECTED_BMATTRIBUTES = 0
EXPECTED_WBYTESPERINTERVAL = 0

GADGET = "gadgets/ss_bulk_loopback"
SIZES = [1, 64, 1024, 4096]
ITERATIONS = 5
TIMEOUT_MS = 2000


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


def _validate_companion(ep):
    """Validate the SS endpoint companion descriptor following ``ep``.

    PyUSB exposes class-/structural-specific descriptors that follow an
    endpoint descriptor via ``ep.extra_descriptors``. For SS endpoints the
    kernel emits exactly the 6-byte SS companion there.
    """
    extra = bytes(ep.extra_descriptors)
    assert len(extra) == SS_COMP_LEN, (
        f"ep 0x{ep.bEndpointAddress:02x} extra_descriptors len={len(extra)} "
        f"(expected {SS_COMP_LEN}): {extra.hex()}")
    assert extra[0] == SS_COMP_LEN, \
        f"SS companion bLength={extra[0]} (expected {SS_COMP_LEN})"
    assert extra[1] == SS_COMP_TYPE, \
        f"SS companion bDescriptorType=0x{extra[1]:02x} (expected 0x30)"
    assert extra[2] == EXPECTED_BMAXBURST, f"bMaxBurst={extra[2]}"
    assert extra[3] == EXPECTED_BMATTRIBUTES, f"bmAttributes={extra[3]}"
    wbpi = extra[4] | (extra[5] << 8)
    assert wbpi == EXPECTED_WBYTESPERINTERVAL, f"wBytesPerInterval={wbpi}"


def _validate_descriptors(dev):
    cfg = dev.get_active_configuration()
    assert cfg.bNumInterfaces == 1, \
        f"expected 1 interface, got {cfg.bNumInterfaces}"

    intf = cfg[(0, 0)]
    assert intf.bInterfaceClass == EXPECTED_INTERFACE_CLASS, \
        f"bInterfaceClass=0x{intf.bInterfaceClass:02x}"
    assert intf.bNumEndpoints == EXPECTED_NUM_ENDPOINTS

    s = usb.util.get_string(dev, intf.iInterface) if intf.iInterface else None
    assert s == EXPECTED_INTERFACE_STRING, \
        f"iInterface={s!r} (expected {EXPECTED_INTERFACE_STRING!r})"

    ep_in = ep_out = None
    for ep in intf:
        attr = ep.bmAttributes & 0x03
        assert attr == 0x02, (
            f"ep 0x{ep.bEndpointAddress:02x} not bulk "
            f"(bmAttributes=0x{ep.bmAttributes:02x})")
        assert ep.wMaxPacketSize == EXPECTED_SS_MAXPACKET, (
            f"ep 0x{ep.bEndpointAddress:02x} mps={ep.wMaxPacketSize} "
            f"(expected {EXPECTED_SS_MAXPACKET})")
        _validate_companion(ep)
        if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_IN:
            assert ep_in is None
            ep_in = ep
        else:
            assert ep_out is None
            ep_out = ep
    assert ep_in is not None and ep_out is not None


def _roundtrip(ep_in, ep_out, length):
    payload = secrets.token_bytes(length)
    chunk = ep_out.wMaxPacketSize
    received = bytearray()
    for off in range(0, length, chunk):
        slice_ = payload[off:off + chunk]
        written = ep_out.write(slice_, timeout=TIMEOUT_MS)
        assert written == len(slice_), \
            f"short write at offset {off}: {written}/{len(slice_)}"
        received += bytes(ep_in.read(len(slice_), timeout=TIMEOUT_MS))
    assert bytes(received) == payload, \
        f"loopback mismatch (sent {length} B, got {len(received)} B)"


@pytest.mark.parametrize("dummy_hcd_speed", ["super-speed"], indirect=True)
@pytest.mark.parametrize("gadget", [GADGET], indirect=True)
def test_descriptors(dummy_hcd_speed, gadget):
    dev = _open_device()
    try:
        _validate_descriptors(dev)
    finally:
        usb.util.dispose_resources(dev)


@pytest.mark.parametrize("dummy_hcd_speed", ["super-speed"], indirect=True)
@pytest.mark.parametrize("gadget", [GADGET], indirect=True)
def test_loopback(dummy_hcd_speed, gadget):
    dev = _open_device()
    intf, ep_in, ep_out = _bulk_endpoints(dev)
    usb.util.claim_interface(dev, intf.bInterfaceNumber)
    try:
        for size in SIZES:
            for _ in range(ITERATIONS):
                _roundtrip(ep_in, ep_out, size)
    finally:
        usb.util.release_interface(dev, intf.bInterfaceNumber)
        usb.util.dispose_resources(dev)
