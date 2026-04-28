/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Robert Baldyga
 *
 * Two independent vendor-class interfaces on a single function, each with
 * its own bulk IN+OUT loopback pair and its own iInterface string.  The
 * smallest gadget that exercises libusbf's multi-interface descriptor and
 * strings tables: separate bInterfaceNumber per interface, independent
 * iInterface indices in the same strings table at LangID 0x0409.
 *
 * Host-side test selects each interface in turn (no alt-setting changes -
 * each interface has a single alt) and runs a bulk loopback against it.
 */

#include <libusbf.h>

#include <stdio.h>
#include <string.h>
#include <errno.h>

#define NUM_INTFS 2
#define BUF_SIZE  512

struct intf_state {
	struct usbf_endpoint *ep_in;
	struct usbf_endpoint *ep_out;
	unsigned char buf[BUF_SIZE];
	int out_armed;
};

static struct intf_state intfs[NUM_INTFS];
static int configured;

static void out_complete(struct usbf_endpoint *ep, void *data,
                         size_t length, ssize_t result, void *user);
static void in_complete(struct usbf_endpoint *ep, void *data,
                        size_t length, ssize_t result, void *user);

static void submit_out(struct intf_state *s)
{
	int ret;

	if (s->out_armed)
		return;
	ret = usbf_submit(s->ep_out, s->buf, sizeof(s->buf), out_complete, s);
	if (ret == 0)
		s->out_armed = 1;
}

static void submit_in(struct intf_state *s, size_t n)
{
	int ret = usbf_submit(s->ep_in, s->buf, n, in_complete, s);
	if (ret < 0)
		fprintf(stderr, "submit IN failed: %s\n", strerror(-ret));
}

static void out_complete(struct usbf_endpoint *ep, void *data,
                         size_t length, ssize_t result, void *user)
{
	struct intf_state *s = user;

	s->out_armed = 0;
	if (result < 0) {
		if (configured)
			submit_out(s);
		return;
	}
	submit_in(s, result);
}

static void in_complete(struct usbf_endpoint *ep, void *data,
                        size_t length, ssize_t result, void *user)
{
	struct intf_state *s = user;

	if (result < 0)
		return;
	submit_out(s);
}

static int event_handler(enum usbf_event_type event)
{
	int i;

	switch (event) {
	case USBF_EVENT_ENABLE:
		configured = 1;
		for (i = 0; i < NUM_INTFS; i++)
			submit_out(&intfs[i]);
		break;
	case USBF_EVENT_DISABLE:
		configured = 0;
		break;
	default:
		break;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	static const char *const names[NUM_INTFS] = {
		"two_interfaces_a",
		"two_interfaces_b",
	};

	struct usbf_function *func;
	int i, ret;

	struct usbf_function_descriptor f_desc = {
		.speed = USBF_SPEED_FS | USBF_SPEED_HS,
		.event_handler = event_handler,
	};

	if (argc != 2) {
		fprintf(stderr, "usage: %s <ffs-mount>\n", argv[0]);
		return 1;
	}

	func = usbf_create_function(&f_desc, argv[1]);
	if (!func) { fprintf(stderr, "create_function failed\n"); return 1; }

	for (i = 0; i < NUM_INTFS; i++) {
		struct usbf_interface *intf;
		struct usbf_alt_setting *alt;
		struct usbf_endpoint_descriptor ep_desc = {
			.type = USBF_BULK,
			.fs_maxpacketsize = 64,
			.hs_maxpacketsize = 512,
		};
		struct usbf_interface_descriptor i_desc = {
			.interface_class = USBF_CLASS_VENDOR_SPEC,
			.string = (char *)names[i],
		};

		intf = usbf_add_interface(func, &i_desc);
		if (!intf) { fprintf(stderr, "add intf %d failed\n", i); return 1; }

		alt = usbf_add_alt_setting(intf);
		if (!alt) { fprintf(stderr, "add alt %d failed\n", i); return 1; }

		ep_desc.direction = USBF_IN;
		intfs[i].ep_in = usbf_add_endpoint(alt, &ep_desc);
		if (!intfs[i].ep_in) {
			fprintf(stderr, "intf %d ep_in failed\n", i);
			return 1;
		}

		ep_desc.direction = USBF_OUT;
		intfs[i].ep_out = usbf_add_endpoint(alt, &ep_desc);
		if (!intfs[i].ep_out) {
			fprintf(stderr, "intf %d ep_out failed\n", i);
			return 1;
		}
	}

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
