/*
 * Copyright (C) 2014 Robert Baldyga
 *
 * Robert Baldyga <r.baldyga@hackerion.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
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
