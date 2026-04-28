# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Robert Baldyga

"""Host-side test for the two_interfaces gadget.

Two independent vendor-class interfaces on a single function, each with its
own bulk IN+OUT loopback pair and its own iInterface string.  Validates that
libusbf emits a multi-interface descriptor blob with distinct
bInterfaceNumber values and per-interface strings, and that I/O on each
interface is independent.
"""

import secrets

import pytest
import usb.core
import usb.util

VID = 0x1d6b
PID = 0x0104

EXPECTED_INTERFACE_CLASS = 0xff
EXPECTED_NUM_INTERFACES = 2

EXPECTED_INTF_STRINGS = {
    0: "two_interfaces_a",
    1: "two_interfaces_b",
}

VALID_BULK_MAXPACKET = (64, 512)

GADGET = "gadgets/two_interfaces"
SIZES = [1, 64, 512, 1024]
ITERATIONS = 3
TIMEOUT_MS = 2000


def _open_device():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    assert dev is not None, \
        f"no device with VID:PID {VID:04x}:{PID:04x} found"
    dev.set_configuration()
    return dev


def _interface(dev, intf_num):
    cfg = dev.get_active_configuration()
    return cfg[(intf_num, 0)]


def _validate_descriptors(dev):
    assert dev.bNumConfigurations == 1
    cfg = dev.get_active_configuration()
    assert cfg.bNumInterfaces == EXPECTED_NUM_INTERFACES, (
        f"expected {EXPECTED_NUM_INTERFACES} interfaces, "
        f"got {cfg.bNumInterfaces}")

    seen_intf_nums = set()
    for intf in cfg:
        assert intf.bAlternateSetting == 0, (
            f"intf {intf.bInterfaceNumber} alt={intf.bAlternateSetting}")
        assert intf.bInterfaceClass == EXPECTED_INTERFACE_CLASS, (
            f"intf {intf.bInterfaceNumber} class="
            f"0x{intf.bInterfaceClass:02x}")
        assert intf.bNumEndpoints == 2, (
            f"intf {intf.bInterfaceNumber} numEPs={intf.bNumEndpoints}")

        s = usb.util.get_string(dev, intf.iInterface) if intf.iInterface else None
        assert s == EXPECTED_INTF_STRINGS[intf.bInterfaceNumber], (
            f"intf {intf.bInterfaceNumber} iInterface={s!r} "
            f"(expected {EXPECTED_INTF_STRINGS[intf.bInterfaceNumber]!r})")

        ep_in = ep_out = None
        for ep in intf:
            assert ep.bmAttributes & 0x03 == 0x02, (
                f"intf {intf.bInterfaceNumber} ep "
                f"0x{ep.bEndpointAddress:02x} not bulk "
                f"(bmAttributes=0x{ep.bmAttributes:02x})")
            assert ep.wMaxPacketSize in VALID_BULK_MAXPACKET, (
                f"intf {intf.bInterfaceNumber} ep "
                f"0x{ep.bEndpointAddress:02x} mps={ep.wMaxPacketSize}")
            if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_IN:
                assert ep_in is None
                ep_in = ep
            else:
                assert ep_out is None
                ep_out = ep
        assert ep_in is not None and ep_out is not None, \
            f"intf {intf.bInterfaceNumber} missing IN or OUT"

        seen_intf_nums.add(intf.bInterfaceNumber)

    assert seen_intf_nums == set(EXPECTED_INTF_STRINGS), (
        f"expected interface numbers {sorted(EXPECTED_INTF_STRINGS)}, "
        f"got {sorted(seen_intf_nums)}")

    # Each interface must have distinct iInterface indices (no reuse).
    iface_strings = {intf.bInterfaceNumber: intf.iInterface for intf in cfg}
    assert len(set(iface_strings.values())) == len(iface_strings), (
        f"interfaces share iInterface indices: {iface_strings}")


def _ep_pair(intf):
    ep_in = usb.util.find_descriptor(
        intf, custom_match=lambda e:
            usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)
    ep_out = usb.util.find_descriptor(
        intf, custom_match=lambda e:
            usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
    return ep_in, ep_out


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


@pytest.mark.parametrize("gadget", [GADGET], indirect=True)
def test_descriptors(gadget):
    dev = _open_device()
    try:
        _validate_descriptors(dev)
    finally:
        usb.util.dispose_resources(dev)


@pytest.mark.parametrize("gadget", [GADGET], indirect=True)
def test_loopback_per_interface(gadget):
    dev = _open_device()
    try:
        for intf_num in sorted(EXPECTED_INTF_STRINGS):
            intf = _interface(dev, intf_num)
            usb.util.claim_interface(dev, intf_num)
            try:
                ep_in, ep_out = _ep_pair(intf)
                for size in SIZES:
                    for _ in range(ITERATIONS):
                        _roundtrip(ep_in, ep_out, size)
            finally:
                usb.util.release_interface(dev, intf_num)
    finally:
        usb.util.dispose_resources(dev)
