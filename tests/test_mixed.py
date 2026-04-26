# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Robert Baldyga

"""Host-side descriptor-only test for the mixed_endpoints gadget.

Mirrors the spec table in gadgets/mixed_endpoints.c and verifies that every
endpoint declared in the gadget shows up on the bus with the right transfer
type, direction, wMaxPacketSize, and bInterval.  No I/O - this test exists
to catch regressions in libusbf's descriptor synthesis.

Endpoint addresses (the ep number bits) are *not* checked against a fixed
table: the kernel UDC layer remaps logical endpoints to physical UDC slots
during composite bind, so the address numbers depend on the controller, not
on libusbf.  Endpoint *order* within the interface is preserved by the
kernel, though, and is what real host drivers key off ("first bulk IN in
interface X"), so we check position-by-position against the spec table.
"""

import pytest
import usb.core
import usb.util

VID = 0x1d6b
PID = 0x0104

EXPECTED_INTERFACE_CLASS = 0xff
EXPECTED_INTERFACE_STRING = "mixed_endpoints"

# Transfer-type codes in bmAttributes & 0x03.
BULK, INTR = 0x02, 0x03

# Mirror of the C-side specs[] table: (type, direction_bit, hs_mps, hs_interval).
# We test against HS values since dummy_hcd negotiates high speed.  No iso
# entries: dummy_hcd doesn't expose iso endpoints, so we can't enumerate them
# in this environment.
EXPECTED = [
    (BULK, usb.util.ENDPOINT_IN,  512, 0),
    (BULK, usb.util.ENDPOINT_OUT, 512, 0),
    (INTR, usb.util.ENDPOINT_IN,   64, 4),
    (INTR, usb.util.ENDPOINT_OUT,  32, 8),
    (INTR, usb.util.ENDPOINT_IN,  128, 6),
    (INTR, usb.util.ENDPOINT_OUT,  16, 4),
]

GADGET = "gadgets/mixed_endpoints"


@pytest.mark.parametrize("gadget", [GADGET], indirect=True)
def test_descriptors(gadget):
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    assert dev is not None, \
        f"no device with VID:PID {VID:04x}:{PID:04x} found"
    dev.set_configuration()
    try:
        assert dev.bNumConfigurations == 1
        cfg = dev.get_active_configuration()
        assert cfg.bNumInterfaces == 1

        alts = [i for i in cfg if i.bInterfaceNumber == 0]
        assert len(alts) == 1, \
            f"expected 1 alt setting, got {[i.bAlternateSetting for i in alts]}"
        intf = alts[0]
        assert intf.bInterfaceClass == EXPECTED_INTERFACE_CLASS
        assert intf.bNumEndpoints == len(EXPECTED), (
            f"expected {len(EXPECTED)} endpoints, got {intf.bNumEndpoints}")

        iface_str = usb.util.get_string(dev, intf.iInterface) \
            if intf.iInterface else None
        assert iface_str == EXPECTED_INTERFACE_STRING, \
            f"iInterface string {iface_str!r} != {EXPECTED_INTERFACE_STRING!r}"

        # Compare position-by-position: pyusb iterates endpoints in the
        # order they appear in the interface descriptor, which the kernel
        # preserves across UDC remapping.  Address numbers themselves vary
        # with the UDC, so they're not in the tuple.
        actual = [(ep.bmAttributes & 0x03,
                   usb.util.endpoint_direction(ep.bEndpointAddress),
                   ep.wMaxPacketSize,
                   ep.bInterval) for ep in intf]
        assert actual == EXPECTED, (
            f"endpoint sequence mismatch:\n"
            f"  expected: {EXPECTED}\n"
            f"  actual:   {actual}")
    finally:
        usb.util.dispose_resources(dev)
