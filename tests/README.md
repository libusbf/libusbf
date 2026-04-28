# Tests

Test infrastructure for libusbf.  Uses `dummy_hcd` (Linux's loopback USB host
controller) so tests run on a regular Linux box without real hardware.

## Prerequisites

- `dummy_hcd` loaded (`sudo modprobe dummy_hcd`).  Provides `dummy_udc.0` under
  `/sys/class/udc/`.
- libusbf built (`./autogen.sh && ./configure && make` in the libusbf root,
  which produces `src/.libs/libusbf.a`).
- Run as root (configfs/functionfs operations require it).

## Layout

- `setup.sh` - bring up a gadget end-to-end (configfs + FFS + UDC bind), run a
  test gadget binary, and tear everything down on exit.
- `teardown.sh` - idempotent cleanup, useful when `setup.sh` died ungracefully.
- `gadgets/` - small C programs exercising different libusbf API patterns.
- `conftest.py`, `test_*.py` - pytest orchestration: each test spawns
  `setup.sh` for its gadget, waits for the device to enumerate, drives it
  over libusb, and SIGINTs the setup script on teardown.

## Running the suite

Build the gadgets, then run pytest as root (configfs/functionfs and raw
libusb access both need it).  Needs `python3-pytest` and `python3-usb`.

```
cd libusbf/tests/gadgets && make
cd ..
sudo python3 -m pytest -v
```

Each test brings the gadget up and tears it down again - ~1s of overhead
per test for the isolation.

## Test gadgets

| Gadget                 | Shape                                                  | Test                       | Purpose                                  |
|------------------------|--------------------------------------------------------|----------------------------|------------------------------------------|
| `bulk_loopback`        | 1 intf, 1 alt, bulk IN+OUT, 512-byte loopback          | `test_bulk.py`             | bulk transfer path                       |
| `control_loopback`     | 1 intf, 1 alt, no data eps; vendor SETUP loopback      | `test_control.py`          | ep0 / setup_handler path                 |
| `interrupt_loopback`   | 1 intf, 1 alt, interrupt IN+OUT, 64-byte loopback      | `test_interrupt.py`        | interrupt transfer path                  |
| `mixed_endpoints`      | 1 intf, 1 alt, 6 eps (bulk + intr, varied mps/intr)    | `test_mixed.py`            | descriptor synthesis correctness         |
| `altsetting_loopback`  | 1 intf, 3 alts: interrupt IN+OUT, varied mps/interval  | `test_altsetting.py`       | multi-alt + SET_INTERFACE path           |
| `two_interfaces`       | 2 intfs, each with 1 alt, bulk IN+OUT loopback         | `test_two_interfaces.py`   | multi-interface + per-interface strings  |
| `hid_minimal`          | 1 HID intf, HID class descriptor + 1 interrupt-IN ep   | `test_hid.py`              | class-specific descriptor emission       |
| `ss_bulk_loopback`     | SS-only, 1 intf, bulk IN+OUT loopback at 1024-byte mps | `test_ss.py`               | SS endpoint companion descriptor (skipped on HS UDC) |
| `halt_loopback`        | 1 intf, bulk IN+OUT loopback + halt/clear-halt control | `test_halt.py`             | usbf_halt + usbf_clear_halt round-trip   |
| `cancel_loopback`      | 1 intf, bulk IN+OUT, pre-submits OUT buffers           | `test_cancel.py`           | usbf_cancel + usbf_cancel_all round-trip |
| `iad_minimal`          | 2 intfs, no eps, IAD wrapping both                     | `test_iad.py`              | usbf_set_iad descriptor + iFunction string |
| `all_ctrl_recip`       | 1 intf, no eps; vendor/Device ep0 loopback             | `test_all_ctrl_recip.py`   | USBF_ALL_CTRL_RECIP forwards Device-recipient ctrl |
| `speed_query`          | 1 intf, no eps; vendor IN returns usbf_get_speed result | `test_speed.py`           | usbf_get_speed across FS/HS/SS |

## Isochronous endpoints

`dummy_hcd` does not implement isochronous transfers, so iso endpoints
cannot be exercised by this suite.  libusbf supports them, but they need
real hardware to test - run a gadget that declares iso endpoints against
a UDC backed by an actual USB controller.

## Per-test UDC speed

Tests that need a specific bus speed use the `dummy_hcd_speed` fixture -
parametrize it with one of `"full-speed"`, `"high-speed"`, or
`"super-speed"` and the suite reloads `dummy_hcd` with the matching
module parameters before bringing the gadget up. `test_ss.py` and
`test_speed.py` exercise this; the rest of the suite runs against the
default high-speed module.

## Tunables

`setup.sh` honours these env vars:

- `GADGET_NAME` (default `test`) - configfs gadget directory name.
- `FFS_MOUNT` (default `/dev/ffs/${GADGET_NAME}`) - FFS mount point.
- `UDC` (default `dummy_udc.0`) - UDC to bind. Tests target `dummy_hcd`;
  override if you want to bind a real UDC.
- `VID` / `PID` (default `0x1d6b` / `0x0104`).
