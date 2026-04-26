# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Robert Baldyga

"""Host-side test for the altsetting_loopback gadget.

Three alt-settings on a single interface, each with an interrupt IN+OUT
loopback pair but differing wMaxPacketSize and bInterval - modelling the
typical altsetting use case (different bandwidth profiles for the same
interface shape).

Two test cases: descriptor validation, and a per-alt walk that selects each
alt via SET_INTERFACE and exercises an interrupt loopback against the
endpoint pair belonging to that alt.  The walk is the actual coverage gain
over the single-alt tests - descriptor synthesis is partly covered by
mixed_endpoints, but this is the only test that drives I/O across an alt
transition with diverging endpoint parameters per alt.
"""

import secrets

import pytest
import usb.core
import usb.util

VID = 0x1d6b
PID = 0x0104

INTERFACE_NUM = 0
EXPECTED_INTERFACE_CLASS = 0xff
EXPECTED_INTERFACE_STRING = "altsetting_loopback"

# Per-alt expected (wMaxPacketSize, bInterval) at the negotiated speed.
# dummy_hcd presents as high-speed; HS interrupt bInterval is encoded as
# 2^(bInterval-1) microframes, which is what the descriptor carries.
ALT_CONFIG = {
    0: {"mps": 8,  "interval": 1},
    1: {"mps": 32, "interval": 4},
    2: {"mps": 64, "interval": 8},
}

GADGET = "gadgets/altsetting_loopback"
ITERATIONS = 3
TIMEOUT_MS = 2000


def _open_device():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    assert dev is not None, \
        f"no device with VID:PID {VID:04x}:{PID:04x} found"
    dev.set_configuration()
    return dev


def _alts(dev):
    cfg = dev.get_active_configuration()
    return sorted(
        (i for i in cfg if i.bInterfaceNumber == INTERFACE_NUM),
        key=lambda i: i.bAlternateSetting)


def _validate_descriptors(dev):
    assert dev.bNumConfigurations == 1
    cfg = dev.get_active_configuration()
    assert cfg.bNumInterfaces == 1, \
        f"expected 1 interface, got {cfg.bNumInterfaces}"

    alts = _alts(dev)
    assert [a.bAlternateSetting for a in alts] == sorted(ALT_CONFIG), (
        f"expected alt-settings {sorted(ALT_CONFIG)}, got "
        f"{[a.bAlternateSetting for a in alts]}")

    for a in alts:
        expected = ALT_CONFIG[a.bAlternateSetting]
        assert a.bInterfaceClass == EXPECTED_INTERFACE_CLASS, (
            f"alt {a.bAlternateSetting} bInterfaceClass="
            f"0x{a.bInterfaceClass:02x}")
        s = usb.util.get_string(dev, a.iInterface) if a.iInterface else None
        assert s == EXPECTED_INTERFACE_STRING, (
            f"alt {a.bAlternateSetting} iInterface={s!r}")
        assert a.bNumEndpoints == 2, (
            f"alt {a.bAlternateSetting} bNumEndpoints={a.bNumEndpoints}")

        ep_in = ep_out = None
        for ep in a:
            assert ep.bmAttributes & 0x03 == 0x03, (
                f"alt {a.bAlternateSetting} ep 0x{ep.bEndpointAddress:02x} "
                f"not interrupt (bmAttributes=0x{ep.bmAttributes:02x})")
            assert ep.wMaxPacketSize == expected["mps"], (
                f"alt {a.bAlternateSetting} ep 0x{ep.bEndpointAddress:02x} "
                f"mps={ep.wMaxPacketSize} (expected {expected['mps']})")
            assert ep.bInterval == expected["interval"], (
                f"alt {a.bAlternateSetting} ep 0x{ep.bEndpointAddress:02x} "
                f"bInterval={ep.bInterval} (expected {expected['interval']})")
            if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_IN:
                assert ep_in is None
                ep_in = ep
            else:
                assert ep_out is None
                ep_out = ep
        assert ep_in is not None and ep_out is not None, \
            f"alt {a.bAlternateSetting} missing interrupt IN or OUT endpoint"


def _ep_pair(intf):
    ep_in = usb.util.find_descriptor(
        intf, custom_match=lambda e:
            usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)
    ep_out = usb.util.find_descriptor(
        intf, custom_match=lambda e:
            usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
    return ep_in, ep_out


def _data_roundtrip(ep_in, ep_out, length):
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


@pytest.mark.parametrize("gadget", [GADGET], indirect=True)
def test_descriptors(gadget):
    dev = _open_device()
    try:
        _validate_descriptors(dev)
    finally:
        usb.util.dispose_resources(dev)


@pytest.mark.parametrize("gadget", [GADGET], indirect=True)
def test_loopback_per_alt(gadget):
    dev = _open_device()
    alts = _alts(dev)
    usb.util.claim_interface(dev, INTERFACE_NUM)
    try:
        for alt in alts:
            dev.set_interface_altsetting(interface=INTERFACE_NUM,
                                         alternate_setting=alt.bAlternateSetting)
            ep_in, ep_out = _ep_pair(alt)
            mps = ep_out.wMaxPacketSize
            # 1B (sub-mps), exactly mps, and 2*mps (multi-packet) - uniform
            # coverage across alts regardless of the alt's mps.
            for size in (1, mps, mps * 2):
                for _ in range(ITERATIONS):
                    _data_roundtrip(ep_in, ep_out, size)
    finally:
        usb.util.release_interface(dev, INTERFACE_NUM)
        usb.util.dispose_resources(dev)
