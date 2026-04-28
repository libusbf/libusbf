/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Robert Baldyga
 *
 * Vendor-class gadget that calls usbf_get_speed() and exposes the result via
 * a vendor IN setup request. Used to verify libusbf's negotiated-speed
 * query lines up with what the kernel reports under /sys/class/udc.
 *
 *   bRequestType=0xC1 (vendor, IN, interface), bRequest=0x01:
 *     gadget returns one byte: usbf_get_speed() at the time of the call,
 *     i.e. one of USBF_SPEED_FS (0x01) / HS (0x02) / SS (0x04), or 0.
 */

#include <libusbf.h>

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#define SPEED_REQUEST 0x01

static struct usbf_function *func;

static int setup_handler(const struct usbf_setup_request *setup)
{
	uint8_t type = setup->bRequestType & 0x60;
	uint8_t recipient = setup->bRequestType & 0x1f;
	uint8_t direction = setup->bRequestType & 0x80;
	uint8_t reply;
	ssize_t ret;

	if (type != 0x40 || recipient != 0x01 || !direction ||
	    setup->bRequest != SPEED_REQUEST || setup->wLength < 1)
		return usbf_setup_stall(setup);

	reply = (uint8_t)usbf_get_speed(func);
	ret = usbf_setup_response(setup, &reply, 1);
	if (ret < 0)
		return usbf_setup_stall(setup);
	return 0;
}

int main(int argc, char *argv[])
{
	struct usbf_interface *intf;
	struct usbf_alt_setting *alt;
	int ret;

	struct usbf_function_descriptor f_desc = {
		.speed = USBF_SPEED_FS | USBF_SPEED_HS | USBF_SPEED_SS,
		.setup_handler = setup_handler,
	};

	struct usbf_interface_descriptor i_desc = {
		.interface_class = USBF_CLASS_VENDOR_SPEC,
		.string = "speed_query",
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
