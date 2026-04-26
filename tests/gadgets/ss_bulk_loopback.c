/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2015 Robert Baldyga
 *
 * SuperSpeed-only bulk IN+OUT loopback. Counterpart to bulk_loopback for the
 * SS code path: declares USBF_SPEED_SS, sets ss_maxpacketsize = 1024
 * (spec-mandated for SS bulk), and emits a default SS endpoint companion
 * descriptor (no burst, no streams, wBytesPerInterval = 0 - the latter is
 * required by spec for bulk).
 *
 * Requires a SuperSpeed UDC. dummy_hcd is HS by default; load with
 * `modprobe dummy_hcd is_super_speed=1` to expose dummy_udc.0 as SS. The
 * pytest harness skips the matching test when the UDC isn't SS-capable.
 */

#include <libusbf.h>

#include <stdio.h>
#include <string.h>
#include <errno.h>

static struct usbf_endpoint *ep_in, *ep_out;
static unsigned char buf[1024];
static int enabled;

static void out_complete(struct usbf_endpoint *ep, void *data, size_t length,
                         ssize_t result, void *user);
static void in_complete(struct usbf_endpoint *ep, void *data, size_t length,
                        ssize_t result, void *user);

static void submit_out(void)
{
	int ret = usbf_submit(ep_out, buf, sizeof(buf), out_complete, NULL);
	if (ret < 0)
		fprintf(stderr, "submit OUT failed: %s\n", strerror(-ret));
}

static void submit_in(size_t n)
{
	int ret = usbf_submit(ep_in, buf, n, in_complete, NULL);
	if (ret < 0)
		fprintf(stderr, "submit IN failed: %s\n", strerror(-ret));
}

static void out_complete(struct usbf_endpoint *ep, void *data, size_t length,
                         ssize_t result, void *user)
{
	if (!enabled || result < 0)
		return;
	submit_in(result);
}

static void in_complete(struct usbf_endpoint *ep, void *data, size_t length,
                        ssize_t result, void *user)
{
	if (!enabled || result < 0)
		return;
	submit_out();
}

static int event_handler(enum usbf_event_type event)
{
	switch (event) {
	case USBF_EVENT_ENABLE:
		enabled = 1;
		submit_out();
		break;
	case USBF_EVENT_DISABLE:
		enabled = 0;
		break;
	default:
		break;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	struct usbf_function *func;
	struct usbf_interface *intf;
	struct usbf_alt_setting *alt;
	int ret;

	struct usbf_function_descriptor f_desc = {
		.speed = USBF_SPEED_SS,
		.event_handler = event_handler,
	};

	struct usbf_interface_descriptor i_desc = {
		.interface_class = USBF_CLASS_VENDOR_SPEC,
		.string = "ss_bulk_loopback",
	};

	struct usbf_endpoint_descriptor ep_desc = {
		.type = USBF_BULK,
		.ss_maxpacketsize = 1024,
		/* SS companion: defaults are valid for bulk (no burst, no
		 * streams, wBytesPerInterval = 0 reserved for bulk). */
	};

	if (argc != 2) {
		fprintf(stderr, "usage: %s <ffs-mount>\n", argv[0]);
		return 1;
	}

	func = usbf_create_function(&f_desc, argv[1]);
	if (!func) { fprintf(stderr, "create_function failed\n"); return 1; }

	intf = usbf_add_interface(func, &i_desc);
	if (!intf) { fprintf(stderr, "add_interface failed\n"); return 1; }

	alt = usbf_add_alt_setting(intf);
	if (!alt) { fprintf(stderr, "add_alt_setting failed\n"); return 1; }

	ep_desc.direction = USBF_IN;
	ep_in = usbf_add_endpoint(alt, &ep_desc);
	if (!ep_in) { fprintf(stderr, "add ep_in failed\n"); return 1; }

	ep_desc.direction = USBF_OUT;
	ep_out = usbf_add_endpoint(alt, &ep_desc);
	if (!ep_out) { fprintf(stderr, "add ep_out failed\n"); return 1; }

	ret = usbf_start(func);
	if (ret < 0) {
		fprintf(stderr, "usbf_start: %s\n", strerror(-ret));
		return 1;
	}

	ret = usbf_run(func);
	if (ret < 0)
		fprintf(stderr, "usbf_run: %s\n", strerror(-ret));

	usbf_stop(func);
	usbf_delete_function(func);
	return ret < 0 ? 1 : 0;
}
