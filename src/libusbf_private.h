/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2014 Robert Baldyga
 */

#ifndef __LIBUSBF_PRIVATE_H__
#define __LIBUSBF_PRIVATE_H__

#include <libusbf.h>

#include <stdio.h>
#include <stdint.h>

#include <linux/usb/functionfs.h>

#define MAX_ENDPOINTS 16

struct usbf_endpoint {
	struct usbf_endpoint_descriptor desc;
	uint8_t address;
	int epfile;
};

struct usbf_function {
	struct usbf_function_descriptor desc;
	char *ffs_path;
	uint32_t flags;
	struct usbf_endpoint *endpoints[MAX_ENDPOINTS];
	int ep_count;
	int ep0_file;
};

#endif /* __LIBUSBF_PRIVATE_H__ */
