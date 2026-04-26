# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Robert Baldyga

"""Host-side test for the iad_minimal gadget.

Walks the raw configuration descriptor and verifies an Interface Association
Descriptor is emitted before the first interface descriptor with the values
the gadget set via usbf_set_iad: function class/subclass/protocol, the
correct bFirstInterface (0) and bInterfaceCount (2), and the iFunction
string.
"""

import pytest
import usb.core
import usb.util

VID = 0x1d6b
PID = 0x0104

GADGET = "gadgets/iad_minimal"
TIMEOUT_MS = 1000

USB_DT_INTERFACE_ASSOCIATION = 0x0b
USB_DT_INTERFACE = 0x04
USB_DT_CONFIG = 0x02

EXPECTED_FUNCTION_CLASS    = 0xab
EXPECTED_FUNCTION_SUBCLASS = 0xcd
EXPECTED_FUNCTION_PROTOCOL = 0xef
EXPECTED_IAD_STRING        = "iad_group"
EXPECTED_INTERFACE_COUNT   = 2


def _open_device():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    assert dev is not None, \
        f"no device with VID:PID {VID:04x}:{PID:04x} found"
    dev.set_configuration()
    return dev


def _read_config_descriptor(dev):
    """GET_DESCRIPTOR(Configuration) -> raw bytes including all sub-descriptors."""
    # First fetch the 9-byte config header to learn wTotalLength.
    head = dev.ctrl_transfer(0x80, 0x06, (USB_DT_CONFIG << 8) | 0, 0, 9, TIMEOUT_MS)
    assert len(head) == 9
    assert head[1] == USB_DT_CONFIG
    total = head[2] | (head[3] << 8)
    full = dev.ctrl_transfer(0x80, 0x06, (USB_DT_CONFIG << 8) | 0, 0, total, TIMEOUT_MS)
    assert len(full) == total, f"short config descriptor: {len(full)}/{total}"
    return bytes(full)


def _walk(blob):
    """Yield (bDescriptorType, body) for each descriptor in the blob."""
    off = 0
    while off < len(blob):
        length = blob[off]
        assert length >= 2, f"bad bLength at offset {off}: {length}"
        dtype = blob[off + 1]
        yield dtype, blob[off:off + length]
        off += length


@pytest.mark.parametrize("gadget", [GADGET], indirect=True)
def test_iad_descriptor(gadget):
    dev = _open_device()
    try:
        blob = _read_config_descriptor(dev)

        descriptors = list(_walk(blob))
        types = [t for t, _ in descriptors]

        # IAD must precede the first interface descriptor.
        assert USB_DT_INTERFACE_ASSOCIATION in types, \
            f"no IAD found in config descriptor (types={[hex(t) for t in types]})"
        iad_idx = types.index(USB_DT_INTERFACE_ASSOCIATION)
        intf_idx = types.index(USB_DT_INTERFACE)
        assert iad_idx < intf_idx, \
            f"IAD at idx {iad_idx} not before first interface at {intf_idx}"

        iad = descriptors[iad_idx][1]
        assert len(iad) == 8, f"IAD bLength={len(iad)}, expected 8"
        bFirstInterface  = iad[2]
        bInterfaceCount  = iad[3]
        bFunctionClass   = iad[4]
        bFunctionSubClass = iad[5]
        bFunctionProtocol = iad[6]
        iFunction        = iad[7]

        assert bFirstInterface == 0, f"bFirstInterface={bFirstInterface}"
        assert bInterfaceCount == EXPECTED_INTERFACE_COUNT, \
            f"bInterfaceCount={bInterfaceCount}"
        assert bFunctionClass == EXPECTED_FUNCTION_CLASS, \
            f"bFunctionClass=0x{bFunctionClass:02x}"
        assert bFunctionSubClass == EXPECTED_FUNCTION_SUBCLASS, \
            f"bFunctionSubClass=0x{bFunctionSubClass:02x}"
        assert bFunctionProtocol == EXPECTED_FUNCTION_PROTOCOL, \
            f"bFunctionProtocol=0x{bFunctionProtocol:02x}"

        assert iFunction != 0, "iFunction not assigned"
        s = usb.util.get_string(dev, iFunction)
        assert s == EXPECTED_IAD_STRING, \
            f"iFunction string {s!r}, expected {EXPECTED_IAD_STRING!r}"
    finally:
        usb.util.dispose_resources(dev)


@pytest.mark.parametrize("gadget", [GADGET], indirect=True)
def test_interface_strings_still_resolve(gadget):
    """Adding the IAD must not break the interface-string indexing - each
    interface should still resolve its iInterface to the right name."""
    dev = _open_device()
    try:
        cfg = dev.get_active_configuration()
        seen = {}
        for intf in cfg:
            assert intf.iInterface != 0, \
                f"intf {intf.bInterfaceNumber} has no iInterface"
            seen[intf.bInterfaceNumber] = usb.util.get_string(
                dev, intf.iInterface)
        assert seen == {0: "iad_minimal_a", 1: "iad_minimal_b"}, \
            f"interface strings: {seen}"
    finally:
        usb.util.dispose_resources(dev)
