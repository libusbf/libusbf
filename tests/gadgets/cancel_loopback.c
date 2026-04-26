/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Robert Baldyga
 *
 * Bulk OUT/IN gadget for testing usbf_cancel() and usbf_cancel_all().
 *
 * On enable, pre-submits NUM_PRE_SUBMITS OUT buffers and does NOT auto-resubmit
 * on completion: every recorded callback comes from the host-driven cancel
 * path. The IN endpoint is declared so the function looks like a typical bulk
 * pair, but no IN submits are queued.
 *
 * Vendor requests on the interface recipient (interface 0):
 *
 *   bRequestType=0x41, bRequest=0x01, wValue=ep number
 *       gadget calls usbf_cancel() on that endpoint
 *
 *   bRequestType=0x41, bRequest=0x02
 *       gadget calls usbf_cancel_all() on the function
 *
 *   bRequestType=0xC1, bRequest=0x03, wLength>=8
 *       gadget responds with the 8-byte stats struct (see below)
 *
 * Anything else stalls.
 */

#include <libusbf.h>

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#define NUM_PRE_SUBMITS 4
#define BUF_SIZE        512

#define VR_CANCEL_EP   0x01
#define VR_CANCEL_ALL  0x02
#define VR_GET_STATS   0x03

static struct usbf_endpoint *ep_in, *ep_out;
static unsigned char buf[NUM_PRE_SUBMITS][BUF_SIZE];

/* Host-readable stats. dummy_hcd is single-host so byte order matches. */
static struct {
	int32_t  cancel_returned;  /* return value of last usbf_cancel* call */
	uint16_t cb_count;         /* OUT completion callbacks fired */
	int16_t  last_result;      /* result of last fired callback (errno fits) */
} stats;

static void out_complete(struct usbf_endpoint *ep, void *data, size_t length,
                         ssize_t result, void *user)
{
	stats.cb_count++;
	stats.last_result = (int16_t)result;
}

static void submit_outs(void)
{
	int i, ret;

	for (i = 0; i < NUM_PRE_SUBMITS; ++i) {
		ret = usbf_submit(ep_out, buf[i], BUF_SIZE,
		                  out_complete, NULL);
		if (ret < 0)
			fprintf(stderr, "submit OUT %d failed: %s\n",
			        i, strerror(-ret));
	}
}

static int event_handler(enum usbf_event_type event)
{
	switch (event) {
	case USBF_EVENT_ENABLE:
		submit_outs();
		break;
	default:
		break;
	}
	return 0;
}

static void reset_stats(void)
{
	memset(&stats, 0, sizeof(stats));
}

static int setup_handler(const struct usbf_setup_request *setup)
{
	uint8_t type = setup->bRequestType & 0x60;
	uint8_t recipient = setup->bRequestType & 0x1f;
	uint8_t direction = setup->bRequestType & 0x80;
	struct usbf_endpoint *target;
	ssize_t ret;

	if (type != 0x40 || recipient != 0x01)
		return usbf_setup_stall(setup);

	if (!direction) {
		switch (setup->bRequest) {
		case VR_CANCEL_EP:
			target = usbf_find_endpoint(setup->function,
			                            setup->wValue);
			if (!target)
				return usbf_setup_stall(setup);
			reset_stats();
			stats.cancel_returned = usbf_cancel(target);
			return usbf_setup_ack(setup);
		case VR_CANCEL_ALL:
			reset_stats();
			stats.cancel_returned =
				usbf_cancel_all(setup->function);
			return usbf_setup_ack(setup);
		default:
			return usbf_setup_stall(setup);
		}
	}

	if (setup->bRequest == VR_GET_STATS) {
		if (setup->wLength < sizeof(stats))
			return usbf_setup_stall(setup);
		ret = usbf_setup_response(setup, &stats, sizeof(stats));
		if (ret < 0)
			return usbf_setup_stall(setup);
		return 0;
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
		.string = "cancel_loopback",
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
