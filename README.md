# libusbf

[![Build](https://github.com/libusbf/libusbf/actions/workflows/build.yml/badge.svg)](https://github.com/libusbf/libusbf/actions/workflows/build.yml)
[![Tests](https://github.com/libusbf/libusbf/actions/workflows/tests.yml/badge.svg)](https://github.com/libusbf/libusbf/actions/workflows/tests.yml)
[![License](https://img.shields.io/badge/License-LGPL_v2.1-blue.svg)](LICENSE)

C library that simplifies writing the device side of USB functions on Linux
by wrapping the FunctionFS API.  It's the device-side counterpart to libusb
(which talks *to* USB devices from the host); not to be confused with
[libusbg](https://github.com/libusbg/libusbg), which composes USB gadgets
through configfs.

The library handles USB descriptor synthesis from a small declarative API
and provides an asynchronous event loop (libaio + epoll) for endpoint I/O.

The codebase is approaching its 1.0 release: the public API is settling
into its final shape and is covered by an integration test suite.

## Requirements

- Linux >= 3.16 (FunctionFS descriptors v2)
- libaio (`apt install libaio-dev`)
- autotools + libtool (for building from source)

## Building

```sh
./autogen.sh
./configure
make
```

This produces `src/.libs/libusbf.{a,so}`.

## Install

```sh
sudo make install
sudo ldconfig
```

By default this installs `libusbf.{a,so}` under `/usr/local/lib` and
`libusbf.h` under `/usr/local/include`.  Pass `--prefix=...` to `./configure`
to install elsewhere, or `DESTDIR=...` to `make install` for staged installs
(e.g. for packaging).  Uninstall with `sudo make uninstall`.

## Quick start

```c
#include <libusbf.h>

struct usbf_function_descriptor f_desc = {
    .speed = USBF_SPEED_FS | USBF_SPEED_HS,
};

struct usbf_interface_descriptor i_desc = {
    .interface_class = USBF_CLASS_VENDOR_SPEC,
    .string = "my interface",
};

struct usbf_endpoint_descriptor ep_desc = {
    .type = USBF_BULK,
    .direction = USBF_IN,
    .fs_maxpacketsize = 64,
    .hs_maxpacketsize = 512,
};

struct usbf_function *func    = usbf_create_function(&f_desc, "/dev/ffs/my");
struct usbf_interface *intf   = usbf_add_interface(func, &i_desc);
struct usbf_alt_setting *alt  = usbf_add_alt_setting(intf);
struct usbf_endpoint *ep      = usbf_add_endpoint(alt, &ep_desc);

usbf_start(func);
usbf_run(func);            /* blocks; event loop dispatches I/O completions */
usbf_stop(func);
usbf_delete_function(func);
```

A function may declare multiple interfaces by calling `usbf_add_interface`
more than once; each interface has its own `bInterfaceNumber`, alt-settings,
and `iInterface` string.  `usbf_submit()` issues an async transfer and fires
the registered completion callback when it lands.  For external event loops,
use `usbf_get_fd()` + `usbf_dispatch()` instead of `usbf_run()`.  See
[`tests/gadgets/`](tests/gadgets/) for worked examples - bulk, interrupt,
and control transfers; multi-alt and multi-interface layouts; HID class
descriptors, IAD, endpoint halt, request cancellation, SuperSpeed
companions, etc. - and [`include/libusbf.h`](include/libusbf.h) for the
full API.

## Threading

libusbf is single-threaded by design: every callback (`event_handler`,
`setup_handler`, and per-submit completion) fires from the same thread
that drives the event loop, so user code needs no locking around libusbf
state.  `usbf_stop()` is safe from any callback; for a cross-thread stop
the caller must coordinate the loop's exit before resource teardown -
see the `usbf_stop` doc in [`include/libusbf.h`](include/libusbf.h).

## Testing

The [`tests/`](tests/) directory holds a pytest-driven suite that brings up
test gadgets under `dummy_hcd` (Linux's loopback USB host controller) and
exercises them over libusb from the host - no real hardware required.  See
[`tests/README.md`](tests/README.md) for prerequisites and run instructions.

## Maintainer

[Robert Baldyga](https://github.com/robertbaldyga).  Bug reports, patches,
and questions go through GitHub issues and pull requests.

## License

LGPL-2.1-or-later - see [LICENSE](LICENSE).
