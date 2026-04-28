/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Robert Baldyga
 *
 * Three alt-settings on a single interface, each with an interrupt IN+OUT
 * loopback pair but differing wMaxPacketSize and bInterval.  Mirrors the
 * typical altsetting use-case in real classes (audio/video bandwidth
 * profiles): same interface shape, different per-alt timing/throughput.
 *
 *   alt 0 - interrupt IN+OUT, mps=8,  hs_interval=1 (125us), fs_interval=1
 *   alt 1 - interrupt IN+OUT, mps=32, hs_interval=4 (1 ms),  fs_interval=4
 *   alt 2 - interrupt IN+OUT, mps=64, hs_interval=8 (16 ms), fs_interval=16
 *
 * Exercises libusbf's multi-alt code path: descriptor synthesis across alts
 * with diverging endpoint parameters, sequential ep file allocation, and
 * ENABLE delivery on each set_alt.
 *
 * FunctionFS emits FUNCTIONFS_ENABLE on every host SET_INTERFACE (not just
 * SET_CONFIGURATION) but never tells userspace which alt is now active, so
 * we keep an OUT submit pending on every alt's OUT endpoint at all times.
 * On any alt switch the kernel cancels in-flight submits with -ESHUTDOWN
 * (and wakes inactive-alt waiters with the same), so we self-heal by
 * re-submitting on every negative completion while the function is
 * configured.  Submits on inactive alts sit in FFS's per-ep wait until that
 * alt becomes active.
 *
 * The OUT submit length is sized to one mps so a single host packet
 * completes the read - for an aio read larger than mps, the kernel only
 * finalises the request on a short packet (< mps), so a buffer of exactly
 * mps bytes is what makes each host write get echoed back immediately.
 */

#include <libusbf.h>

#include <stdio.h>
#include <string.h>
#include <errno.h>

#define NUM_ALTS 3
#define MAX_MPS  64

struct alt_state {
	struct usbf_endpoint *ep_in;
	struct usbf_endpoint *ep_out;
	unsigned char buf[MAX_MPS];
	size_t mps;
	int out_armed;
};

static struct alt_state alts[NUM_ALTS];
static int configured;

static void out_complete(struct usbf_endpoint *ep, void *data,
                         size_t length, ssize_t result, void *user);
static void in_complete(struct usbf_endpoint *ep, void *data,
                        size_t length, ssize_t result, void *user);

static void submit_out(struct alt_state *a)
{
	int ret;

	if (a->out_armed)
		return;
	ret = usbf_submit(a->ep_out, a->buf, a->mps, out_complete, a);
	if (ret == 0)
		a->out_armed = 1;
}

static void submit_in(struct alt_state *a, size_t n)
{
	int ret = usbf_submit(a->ep_in, a->buf, n, in_complete, a);
	if (ret < 0)
		fprintf(stderr, "submit IN failed: %s\n", strerror(-ret));
}

static void out_complete(struct usbf_endpoint *ep, void *data,
                         size_t length, ssize_t result, void *user)
{
	struct alt_state *a = user;

	a->out_armed = 0;
	if (result < 0) {
		if (configured)
			submit_out(a);
		return;
	}
	submit_in(a, result);
}

static void in_complete(struct usbf_endpoint *ep, void *data,
                        size_t length, ssize_t result, void *user)
{
	struct alt_state *a = user;

	if (result < 0)
		return;
	submit_out(a);
}

static int event_handler(enum usbf_event_type event)
{
	int i;

	switch (event) {
	case USBF_EVENT_ENABLE:
		configured = 1;
		for (i = 0; i < NUM_ALTS; i++)
			submit_out(&alts[i]);
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
	static const struct {
		uint16_t mps;
		uint8_t fs_interval;
		uint8_t hs_interval;
	} cfg[NUM_ALTS] = {
		{ 8,  1,  1 },  /* HS: 2^0 = 1 microframe = 125 us */
		{ 32, 4,  4 },  /* HS: 2^3 = 8 microframes = 1 ms  */
		{ 64, 16, 8 },  /* HS: 2^7 = 128 microframes = 16 ms */
	};

	struct usbf_function *func;
	struct usbf_interface *intf;
	int i, ret;

	struct usbf_function_descriptor f_desc = {
		.speed = USBF_SPEED_FS | USBF_SPEED_HS,
		.event_handler = event_handler,
	};

	struct usbf_interface_descriptor i_desc = {
		.interface_class = USBF_CLASS_VENDOR_SPEC,
		.string = "altsetting_loopback",
	};

	if (argc != 2) {
		fprintf(stderr, "usage: %s <ffs-mount>\n", argv[0]);
		return 1;
	}

	func = usbf_create_function(&f_desc, argv[1]);
	if (!func) { fprintf(stderr, "create_function failed\n"); return 1; }

	intf = usbf_add_interface(func, &i_desc);
	if (!intf) { fprintf(stderr, "add_interface failed\n"); return 1; }

	for (i = 0; i < NUM_ALTS; i++) {
		struct usbf_alt_setting *alt;
		struct usbf_endpoint_descriptor ep_desc = {
			.type = USBF_INTERRUPT,
			.fs_maxpacketsize = cfg[i].mps,
			.hs_maxpacketsize = cfg[i].mps,
			.fs_interval = cfg[i].fs_interval,
			.hs_interval = cfg[i].hs_interval,
		};

		alts[i].mps = cfg[i].mps;

		alt = usbf_add_alt_setting(intf);
		if (!alt) { fprintf(stderr, "add alt %d failed\n", i); return 1; }

		ep_desc.direction = USBF_IN;
		alts[i].ep_in = usbf_add_endpoint(alt, &ep_desc);
		if (!alts[i].ep_in) {
			fprintf(stderr, "alt %d ep_in failed\n", i);
			return 1;
		}

		ep_desc.direction = USBF_OUT;
		alts[i].ep_out = usbf_add_endpoint(alt, &ep_desc);
		if (!alts[i].ep_out) {
			fprintf(stderr, "alt %d ep_out failed\n", i);
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
