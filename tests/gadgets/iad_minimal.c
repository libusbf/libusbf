/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Robert Baldyga
 *
 * Two vendor-class interfaces wrapped in a single Interface Association
 * Descriptor. Smallest gadget that exercises usbf_set_iad: each interface
 * has a single empty alt-setting (no data endpoints) so the IAD itself is
 * the only thing under test.
 */

#include <libusbf.h>

#include <stdio.h>
#include <string.h>
#include <errno.h>

#define NUM_INTFS 2

#define IAD_CLASS    0xab
#define IAD_SUBCLASS 0xcd
#define IAD_PROTOCOL 0xef
#define IAD_STRING   "iad_group"

int main(int argc, char *argv[])
{
	static const char *const names[NUM_INTFS] = {
		"iad_minimal_a",
		"iad_minimal_b",
	};

	struct usbf_function *func;
	int i, ret;

	struct usbf_function_descriptor f_desc = {
		.speed = USBF_SPEED_FS | USBF_SPEED_HS,
	};

	struct usbf_iad_descriptor iad_desc = {
		.function_class = IAD_CLASS,
		.function_subclass = IAD_SUBCLASS,
		.function_protocol = IAD_PROTOCOL,
		.string = IAD_STRING,
	};

	if (argc != 2) {
		fprintf(stderr, "usage: %s <ffs-mount>\n", argv[0]);
		return 1;
	}

	func = usbf_create_function(&f_desc, argv[1]);
	if (!func) { fprintf(stderr, "create_function failed\n"); return 1; }

	ret = usbf_set_iad(func, &iad_desc);
	if (ret < 0) {
		fprintf(stderr, "usbf_set_iad: %s\n", strerror(-ret));
		return 1;
	}

	for (i = 0; i < NUM_INTFS; i++) {
		struct usbf_interface *intf;
		struct usbf_alt_setting *alt;
		struct usbf_interface_descriptor i_desc = {
			.interface_class = USBF_CLASS_VENDOR_SPEC,
			.string = (char *)names[i],
		};

		intf = usbf_add_interface(func, &i_desc);
		if (!intf) { fprintf(stderr, "add intf %d failed\n", i); return 1; }

		alt = usbf_add_alt_setting(intf);
		if (!alt) { fprintf(stderr, "add alt %d failed\n", i); return 1; }
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
