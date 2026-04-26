/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2015 Robert Baldyga
 *
 * Single alt-setting, two interrupt endpoints (IN+OUT), loopback.  Mirror of
 * basic_bulk for the interrupt transfer path.
 */

#include <libusbf.h>

#include <stdio.h>
#include <string.h>
#include <errno.h>

static struct usbf_endpoint *ep_in, *ep_out;
static unsigned char buf[64];
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
		.speed = USBF_SPEED_FS | USBF_SPEED_HS,
		.event_handler = event_handler,
	};

	struct usbf_interface_descriptor i_desc = {
		.interface_class = USBF_CLASS_VENDOR_SPEC,
		.string = "interrupt_loopback",
	};

	struct usbf_endpoint_descriptor ep_desc = {
		.type = USBF_INTERRUPT,
		.fs_maxpacketsize = 64,
		.hs_maxpacketsize = 64,
		.fs_interval = 1,  /* 1 ms */
		.hs_interval = 4,  /* 2^(4-1) = 8 microframes = 1 ms */
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
