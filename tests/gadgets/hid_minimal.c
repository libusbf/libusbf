/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Robert Baldyga
 *
 * Minimal HID-class function: a single interface with class HID, one HID
 * class descriptor between the interface and endpoint descriptors, and one
 * interrupt-IN endpoint.  Exists to exercise libusbf's class-specific
 * descriptor emission path; not a functional HID device - no report
 * descriptor is served (FFS forwards class/vendor setup, not standard
 * GET_DESCRIPTOR), so hid-generic on the host won't bind.  The device still
 * enumerates and the host-side test inspects the configuration descriptor
 * blob to confirm the HID class descriptor lands as expected.
 *
 * The HID class descriptor (9 bytes) declares exactly one report descriptor
 * of length 22; the body bytes match a vendor-defined 8-byte input report
 * collection so anyone reading the blob with `lsusb -v` sees a coherent
 * declaration even though the report descriptor itself is never delivered.
 */

#include <libusbf.h>

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

/* HID 1.11, no country code, one report descriptor of 22 bytes. */
#define REPORT_LEN 22

static const uint8_t hid_desc[9] = {
	0x09,                   /* bLength */
	0x21,                   /* bDescriptorType (HID) */
	0x11, 0x01,             /* bcdHID 0x0111 (HID 1.11), little-endian */
	0x00,                   /* bCountryCode (none) */
	0x01,                   /* bNumDescriptors */
	0x22,                   /* bReportDescType (Report) */
	REPORT_LEN, 0x00,       /* wReportLength = 22, little-endian */
};

int main(int argc, char *argv[])
{
	struct usbf_function *func;
	struct usbf_interface *intf;
	struct usbf_alt_setting *alt;
	int ret;

	struct usbf_function_descriptor f_desc = {
		.speed = USBF_SPEED_FS | USBF_SPEED_HS,
	};

	struct usbf_interface_descriptor i_desc = {
		.interface_class = USBF_CLASS_HID,
		.interface_subclass = 0,
		.interface_protocol = 0,
		.string = "hid_minimal",
	};

	struct usbf_endpoint_descriptor ep_desc = {
		.type = USBF_INTERRUPT,
		.direction = USBF_IN,
		.fs_maxpacketsize = 8,
		.hs_maxpacketsize = 8,
		.fs_interval = 10,  /* 10 ms */
		.hs_interval = 7,   /* 2^6 = 64 microframes = 8 ms */
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

	ret = usbf_add_class_descriptor(alt, hid_desc, sizeof(hid_desc));
	if (ret < 0) {
		fprintf(stderr, "add_class_descriptor: %s\n", strerror(-ret));
		return 1;
	}

	if (!usbf_add_endpoint(alt, &ep_desc)) {
		fprintf(stderr, "add_endpoint failed\n");
		return 1;
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
