/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Robert Baldyga
 *
 * Bulk IN+OUT loopback with host-driven endpoint halt and clear-halt
 * primitives. Exercises usbf_halt() and usbf_clear_halt():
 *
 *   - Vendor request 0x41/0x01 (vendor, OUT, interface): the gadget halts
 *     the endpoint identified by wValue (ep number). The interface recipient
 *     keeps wIndex available for FFS's interface revmap; the ep selector
 *     rides in wValue. After this, the host's next transfer on that
 *     endpoint receives a STALL.
 *
 *   - Standard request 0x02/0x01 (CLEAR_FEATURE on endpoint) with
 *     wValue = 0 (ENDPOINT_HALT): the gadget clears the halt on the
 *     matching endpoint via usbf_clear_halt(). FFS forwards endpoint-
 *     recipient setup requests to userspace, so the gadget owns clearing
 *     the halt rather than the kernel composite layer.
 *
 * Anything else stalls.
 */

#include <libusbf.h>

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#define HALT_REQUEST 0x01

static struct usbf_endpoint *ep_in, *ep_out;
static unsigned char buf[512];
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

static int setup_handler(const struct usbf_setup_request *setup)
{
	uint8_t type = setup->bRequestType & 0x60;
	uint8_t recipient = setup->bRequestType & 0x1f;
	struct usbf_endpoint *target;
	int ret;

	/* Vendor halt: bRequestType = 0x41 (vendor, OUT, interface), with the
	 * endpoint number to halt encoded in wValue (FFS revmaps wIndex on
	 * interface-recipient setups, so the ep selector can't ride there). */
	if (type == 0x40 && recipient == 0x01 &&
	    setup->bRequest == HALT_REQUEST) {
		target = usbf_find_endpoint(setup->function, setup->wValue);
		if (!target)
			return usbf_setup_stall(setup);
		ret = usbf_halt(target);
		if (ret < 0) {
			fprintf(stderr, "usbf_halt: %s\n", strerror(-ret));
			return usbf_setup_stall(setup);
		}
		return usbf_setup_ack(setup);
	}

	/* Standard ClearFeature(ENDPOINT_HALT): bRequestType = 0x02
	 * (standard, OUT, endpoint), bRequest = 0x01, wValue = 0. */
	if (type == 0x00 && recipient == 0x02 &&
	    setup->bRequest == 0x01 && setup->wValue == 0) {
		target = usbf_find_endpoint(setup->function, setup->wIndex);
		if (!target)
			return usbf_setup_stall(setup);
		ret = usbf_clear_halt(target);
		if (ret < 0) {
			fprintf(stderr, "usbf_clear_halt: %s\n", strerror(-ret));
			return usbf_setup_stall(setup);
		}
		return usbf_setup_ack(setup);
	}

	return usbf_setup_stall(setup);
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
		.setup_handler = setup_handler,
	};

	struct usbf_interface_descriptor i_desc = {
		.interface_class = USBF_CLASS_VENDOR_SPEC,
		.string = "halt_loopback",
	};

	struct usbf_endpoint_descriptor ep_desc = {
		.type = USBF_BULK,
		.fs_maxpacketsize = 64,
		.hs_maxpacketsize = 512,
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
