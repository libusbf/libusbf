/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Robert Baldyga
 *
 * Vendor-specific control loopback that uses the Device recipient instead
 * of Interface. Without USBF_ALL_CTRL_RECIP the kernel composite layer
 * keeps Device-recipient setup requests; with the flag, FunctionFS forwards
 * them here. The gadget loops a small payload back through ep0:
 *
 *   bRequestType=0x40 (vendor, OUT, device), bRequest=0x01:
 *     host writes wLength bytes; gadget stores them.
 *   bRequestType=0xC0 (vendor, IN,  device), bRequest=0x01:
 *     gadget returns up to wLength bytes from the stored buffer.
 *
 * Anything else stalls.
 */

#include <libusbf.h>

#include <stdio.h>
#include <string.h>
#include <errno.h>

#define LOOPBACK_REQUEST 0x01
#define BUF_SIZE         256

static unsigned char buf[BUF_SIZE];
static size_t stored_len;

static int setup_handler(const struct usbf_setup_request *setup)
{
	uint8_t type = setup->bRequestType & 0x60;
	uint8_t recipient = setup->bRequestType & 0x1f;
	uint8_t direction = setup->bRequestType & 0x80;
	size_t len;
	ssize_t ret;

	if (type != 0x40 || recipient != 0x00 ||
	    setup->bRequest != LOOPBACK_REQUEST)
		return usbf_setup_stall(setup);

	len = setup->wLength;
	if (len > BUF_SIZE)
		return usbf_setup_stall(setup);

	if (direction) {
		if (len > stored_len)
			len = stored_len;
		ret = usbf_setup_response(setup, buf, len);
	} else {
		ret = usbf_setup_response(setup, buf, len);
		if (ret >= 0)
			stored_len = ret;
	}
	if (ret < 0)
		return usbf_setup_stall(setup);
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
		.flags = USBF_ALL_CTRL_RECIP,
		.setup_handler = setup_handler,
	};

	struct usbf_interface_descriptor i_desc = {
		.interface_class = USBF_CLASS_VENDOR_SPEC,
		.string = "all_ctrl_recip",
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
