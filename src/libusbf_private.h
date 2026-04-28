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
#include <libaio.h>

#define MAX_ENDPOINTS		15
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
};

struct usbf_endpoint {
	struct usbf_endpoint_descriptor desc;
	uint8_t address;
	int epfile;
	struct usbf_function *func;
};

struct usbf_function {
	struct usbf_function_descriptor desc;
	char *ffs_path;
	uint32_t flags;
	struct usbf_endpoint *endpoints[MAX_ENDPOINTS];
	int ep_count;
	int ep0_file;

	/* Event loop state, valid between usbf_start and usbf_stop. */
	int epoll_fd;
	int event_fd;
	io_context_t aio_ctx;
	int running;

	struct usbf_iocb iocb_pool[MAX_INFLIGHT];
	struct usbf_iocb *free_iocb;
};

#endif /* __LIBUSBF_PRIVATE_H__ */
