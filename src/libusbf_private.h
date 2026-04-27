/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2014-2026 Robert Baldyga
 */

#ifndef __LIBUSBF_PRIVATE_H__
#define __LIBUSBF_PRIVATE_H__

#include <libusbf.h>

#include <stdio.h>
#include <stdint.h>

#include <linux/usb/functionfs.h>
#include <libaio.h>

#define MAX_ENDPOINTS		15
#define MAX_ALT_SETTINGS	8
#define MAX_INTERFACES		8
#define MAX_CLASS_DESCS		4
#define IOCBS_PER_ENDPOINT	32
#define MAX_INFLIGHT		(MAX_ENDPOINTS * IOCBS_PER_ENDPOINT)

struct usbf_iocb {
	struct iocb cb;
	struct usbf_endpoint *ep;
	void *data;
	size_t length;
	usbf_completion_cb complete;
	void *user;
	struct usbf_iocb *next_free;
	int in_flight;
};

struct usbf_endpoint {
	struct usbf_endpoint_descriptor desc;
	uint8_t address;
	int epfile;
	struct usbf_function *func;
};

struct usbf_class_descriptor {
	void *data;
	size_t length;
};

struct usbf_alt_setting {
	struct usbf_endpoint *endpoints[MAX_ENDPOINTS];
	int ep_count;
	int alt_num;
	struct usbf_class_descriptor *class_descs[MAX_CLASS_DESCS];
	int class_desc_count;
	struct usbf_interface *intf;
};

struct usbf_interface {
	struct usbf_interface_descriptor desc;
	struct usbf_alt_setting *alts[MAX_ALT_SETTINGS];
	int alt_count;
	int intf_num;
	/* 1-based index into the strings table; 0 if no string. Filled in
	 * during usbf_start when the strings table is laid out. */
	int string_idx;
	struct usbf_function *func;
};

struct usbf_function {
	struct usbf_function_descriptor desc;
	char *ffs_path;
	uint32_t speed_mask;
	struct usbf_interface *interfaces[MAX_INTERFACES];
	int interface_count;
	struct usbf_iad_descriptor iad_desc;
	int has_iad;
	int iad_string_idx;
	int ep0_file;

	/* Event loop state, valid between usbf_start and usbf_stop. */
	int epoll_fd;
	int event_fd;
	int stop_fd;
	io_context_t aio_ctx;
	int running;

	struct usbf_iocb iocb_pool[MAX_INFLIGHT];
	struct usbf_iocb *free_iocb;
};

#endif /* __LIBUSBF_PRIVATE_H__ */
