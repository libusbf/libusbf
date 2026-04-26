/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Robert Baldyga
 *
 * Single alt-setting with a mix of bulk and interrupt endpoints in both
 * directions, with varied wMaxPacketSize and bInterval values.  No I/O -
 * the host-side test only validates that the descriptor blob libusbf
 * synthesizes lands on the bus exactly as declared.
 *
 * (No isochronous endpoints: dummy_hcd doesn't expose any.  ISO descriptor
 * synthesis is exercised on the libusbf code path the same as bulk/intr,
 * so this isn't a coverage gap worth bringing in real hardware for.)
 */

#include <libusbf.h>

#include <stdio.h>
#include <string.h>
#include <errno.h>

struct ep_spec {
	enum usbf_endpoint_type type;
	enum usbf_endpoint_direction direction;
	uint16_t fs_mps;
	uint16_t hs_mps;
	uint8_t fs_interval;
	uint8_t hs_interval;
};

/* Declared in this exact order so endpoint addresses are predictable:
 * the Nth call to usbf_add_endpoint produces ep address N | direction.
 * The host-side test reproduces this table to check every field. */
/* HS bulk wMaxPacketSize is fixed at 512 by the USB 2.0 spec; HS interrupt
 * accepts 1-1024, so the meaningful mps variation across this table sits on
 * the interrupt endpoints. */
static const struct ep_spec specs[] = {
	{ USBF_BULK,      USBF_IN,  64, 512, 0, 0 },  /* ep1 IN  bulk */
	{ USBF_BULK,      USBF_OUT, 64, 512, 0, 0 },  /* ep2 OUT bulk */
	{ USBF_INTERRUPT, USBF_IN,  64,  64, 1, 4 },  /* ep3 IN  intr,  1ms */
	{ USBF_INTERRUPT, USBF_OUT, 32,  32, 8, 8 },  /* ep4 OUT intr,  8ms */
	{ USBF_INTERRUPT, USBF_IN,   8, 128, 4, 6 },  /* ep5 IN  intr, larger mps, 4ms */
	{ USBF_INTERRUPT, USBF_OUT, 16,  16, 2, 4 },  /* ep6 OUT intr,  1ms */
};

int main(int argc, char *argv[])
{
	struct usbf_function *func;
	struct usbf_interface *intf;
	struct usbf_alt_setting *alt;
	size_t i;
	int ret;

	struct usbf_function_descriptor f_desc = {
		.speed = USBF_SPEED_FS | USBF_SPEED_HS,
	};

	struct usbf_interface_descriptor i_desc = {
		.interface_class = USBF_CLASS_VENDOR_SPEC,
		.string = "mixed_endpoints",
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

	for (i = 0; i < sizeof(specs) / sizeof(specs[0]); ++i) {
		struct usbf_endpoint_descriptor desc = {
			.type = specs[i].type,
			.direction = specs[i].direction,
			.fs_maxpacketsize = specs[i].fs_mps,
			.hs_maxpacketsize = specs[i].hs_mps,
			.fs_interval = specs[i].fs_interval,
			.hs_interval = specs[i].hs_interval,
		};
		if (!usbf_add_endpoint(alt, &desc)) {
			fprintf(stderr, "add endpoint %zu failed\n", i);
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
