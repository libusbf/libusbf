# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Robert Baldyga

"""Host-side test for the halt_loopback gadget.

Drives usbf_halt() and usbf_clear_halt() through their full round-trip:

  1. baseline bulk loopback works,
  2. host issues a vendor request that the gadget translates to usbf_halt
     on a chosen endpoint,
  3. the host's next transfer on that endpoint stalls (libusb pipe error),
  4. host issues standard ClearFeature(HALT) - FFS forwards it to the
     gadget's setup_handler, which calls usbf_clear_halt,
  5. loopback resumes.

Both directions are exercised in separate test cases.
"""

import errno
import secrets

import pytest
import usb.core
import usb.util

VID = 0x1d6b
PID = 0x0104

INTERFACE_NUM = 0
HALT_REQUEST = 0x01
GADGET = "gadgets/halt_loopback"
TIMEOUT_MS = 1000


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


def _ep_number(ep):
    return ep.bEndpointAddress & 0x7f


def _send_halt(dev, ep_number):
    """Vendor request: bRequestType=0x41 (vendor/OUT/interface), bRequest=0x01,
    wValue=ep number to halt, wIndex=interface number."""
    dev.ctrl_transfer(0x41, HALT_REQUEST, ep_number, INTERFACE_NUM,
                      b"", TIMEOUT_MS)


def _roundtrip(ep_in, ep_out, length):
    payload = secrets.token_bytes(length)
    written = ep_out.write(payload, TIMEOUT_MS)
    assert written == length
    received = bytes(ep_in.read(length, TIMEOUT_MS))
    assert received == payload, \
        f"loopback mismatch (sent {length} B, got {len(received)} B)"


@pytest.mark.parametrize("gadget", [GADGET], indirect=True)
def test_halt_out(gadget):
    dev = _open_device()
    intf, ep_in, ep_out = _bulk_endpoints(dev)
    usb.util.claim_interface(dev, INTERFACE_NUM)
    try:
        _roundtrip(ep_in, ep_out, 16)

        _send_halt(dev, _ep_number(ep_out))

        with pytest.raises(usb.core.USBError) as exc_info:
            ep_out.write(b"x" * 8, TIMEOUT_MS)
        assert exc_info.value.errno == errno.EPIPE, \
            f"expected EPIPE, got errno={exc_info.value.errno}: {exc_info.value!r}"

        dev.clear_halt(ep_out)

        _roundtrip(ep_in, ep_out, 16)
    finally:
        usb.util.release_interface(dev, INTERFACE_NUM)
        usb.util.dispose_resources(dev)


@pytest.mark.parametrize("gadget", [GADGET], indirect=True)
def test_halt_in(gadget):
    dev = _open_device()
    intf, ep_in, ep_out = _bulk_endpoints(dev)
    usb.util.claim_interface(dev, INTERFACE_NUM)
    try:
        _roundtrip(ep_in, ep_out, 16)

        _send_halt(dev, _ep_number(ep_in))

        with pytest.raises(usb.core.USBError) as exc_info:
            ep_in.read(8, TIMEOUT_MS)
        assert exc_info.value.errno == errno.EPIPE, \
            f"expected EPIPE, got errno={exc_info.value.errno}: {exc_info.value!r}"

        dev.clear_halt(ep_in)

        _roundtrip(ep_in, ep_out, 16)
    finally:
        usb.util.release_interface(dev, INTERFACE_NUM)
        usb.util.dispose_resources(dev)
