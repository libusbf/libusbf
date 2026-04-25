/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2014 Robert Baldyga
 */

#include "libusbf_private.h"
#include "descs.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <endian.h>
#include <linux/limits.h>
#include <sys/poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

/* epoll .data.u32 tags for the two fds we watch */
#define TAG_EP0	0
#define TAG_AIO	1

struct usbf_function *
usbf_create_function(struct usbf_function_descriptor *desc, char *path)
{
	struct usbf_function *func;
	int i;

	if (desc->speed & ~(0x07))
		return NULL;
	if (!(desc->speed & 0x07))
		return NULL;

	func = malloc(sizeof(*func));
	if (!func)
		return NULL;

	func->ffs_path = malloc(strlen(path) + 1);
	if (!func->ffs_path) {
		free(func);
		return NULL;
	}

	strcpy(func->ffs_path, path);
	memcpy(&func->desc, desc, sizeof(*desc));

	func->flags = func->desc.speed;
	func->ep_count = 0;
	func->ep0_file = -1;
	func->epoll_fd = -1;
	func->event_fd = -1;
	func->aio_ctx = 0;
	func->running = 0;

	/* Build the free list linking every iocb slot. */
	func->free_iocb = &func->iocb_pool[0];
	for (i = 0; i < MAX_INFLIGHT - 1; ++i)
		func->iocb_pool[i].next_free = &func->iocb_pool[i + 1];
	func->iocb_pool[MAX_INFLIGHT - 1].next_free = NULL;

	return func;
}

void usbf_delete_function(struct usbf_function *func)
{
	int i;

	for (i = 0; i < func->ep_count; ++i)
		free(func->endpoints[i]);
	free(func->ffs_path);
	free(func);
}

struct usbf_endpoint *usbf_add_endpoint(
	struct usbf_function *func, struct usbf_endpoint_descriptor *desc)
{
	struct usbf_endpoint *ep;

	/* TODO - validate maxpacketsize and interval for selected speeds */
	switch (desc->type) {
	case USBF_ISOCHRONOUS:
	case USBF_BULK:
	case USBF_INTERRUPT:
		break;
	default:
		return NULL;
	}

	if (func->ep_count >= MAX_ENDPOINTS)
		return NULL;

	ep = malloc(sizeof(*ep));
	if (!ep)
		return NULL;

	memcpy(&ep->desc, desc, sizeof(*desc));
	ep->func = func;
	ep->epfile = -1;

	func->endpoints[func->ep_count++] = ep;
	ep->address = func->ep_count | ep->desc.direction;

	return ep;
}

int usbf_start(struct usbf_function *func)
{
	struct __usbf_descs descs;
	struct __usbf_strings strings;
	struct usb_functionfs_descs_head_v2 *descs_header;
	struct usb_interface_descriptor *intf_desc;
	struct usb_endpoint_descriptor_no_audio *ep_desc;
	__le32 *count_ptr;
	struct usb_functionfs_strings_head *strings_header;
	struct usbf_endpoint *ep;
	struct epoll_event eev;
	uint32_t speed;
	char *path;
	int ret, i, j;

	/* We count how many speeds we support */
	descs.speeds = !!(func->flags & USBF_SPEED_FS) +
		!!(func->flags & USBF_SPEED_HS) +
		!!(func->flags & USBF_SPEED_SS);
	descs.endpoints = func->ep_count;
	ret = __usbf_descs_alloc(&descs);
	if (ret)
		return ret;

	descs_header = __usbf_descs_access_header(&descs);
	descs_header->magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
	/* Our speed flags has the same value as in FunctionFS */
	descs_header->flags = htole32(func->flags);
	descs_header->length = htole32(descs.length);

	for (i = 0; i < descs.speeds; ++i) {
		count_ptr = __usbf_descs_access_count(&descs, i);
		*count_ptr = htole32(descs.endpoints + 1);
	}

	speed = 1;
	for (i = 0; i < descs.speeds; ++i) {
		while (!(speed & func->flags))
			speed <<= 1;
		intf_desc = __usbf_descs_access_interface(&descs, i);
		intf_desc->bLength = sizeof(*intf_desc);
		intf_desc->bDescriptorType = USB_DT_INTERFACE;
		intf_desc->bNumEndpoints = descs.endpoints;
		intf_desc->bInterfaceClass = func->desc.interface_class;
		intf_desc->iInterface = 1;
		for (j = 0; j < descs.endpoints; ++j) {
			ep = func->endpoints[j];
			ep_desc = __usbf_descs_access_endpoint(&descs, i, j);
			ep_desc->bLength = sizeof(*ep_desc);
			ep_desc->bDescriptorType = USB_DT_ENDPOINT;
			ep_desc->bEndpointAddress = ep->address;
			ep_desc->bmAttributes = ep->desc.type;
			switch (speed) {
			case USBF_SPEED_FS:
				ep_desc->wMaxPacketSize =
					htole16(ep->desc.fs_maxpacketsize);
				ep_desc->bInterval = ep->desc.fs_interval;
				break;
			case USBF_SPEED_HS:
				ep_desc->wMaxPacketSize =
					htole16(ep->desc.hs_maxpacketsize);
				ep_desc->bInterval = ep->desc.hs_interval;
				break;
			case USBF_SPEED_SS:
				ep_desc->wMaxPacketSize =
					htole16(ep->desc.ss_maxpacketsize);
				ep_desc->bInterval = ep->desc.ss_interval;
				break;
			}
		}
		speed <<= 1;
	}

	/* Single en_US (LangID 0x0409) interface string. The strings table
	 * format supports multiple LangIDs and multiple strings per LangID;
	 * generalizing this requires extending usbf_function_descriptor and
	 * the __usbf_strings layout. No current consumer needs it. */
	strings.str_length = strlen(func->desc.string);
	ret = __usbf_strings_alloc(&strings);
	if (ret < 0)
		goto out;

	strings_header = __usbf_strings_access_header(&strings);
	strings_header->magic = htole32(FUNCTIONFS_STRINGS_MAGIC);
	strings_header->length = strings.length;
	strings_header->str_count = htole32(1);
	strings_header->lang_count = htole32(1);

	__usbf_strings_set_code(&strings, htole16(0x0409));
	__usbf_strings_set_string(&strings, func->desc.string);

	/* We need space for 6 chars - 5 for "/ep##" and 1 for '\0' */
	path = malloc(strlen(func->ffs_path) + 6);
	if (!path) {
		ret = -ENOMEM;
		goto out2;
	}

	sprintf(path, "%s/ep0", func->ffs_path);
	func->ep0_file = open(path, O_RDWR);
	if (func->ep0_file < 0) {
		ret = func->ep0_file;
		goto out3;
	}

	ret = write(func->ep0_file, descs.data, descs.length);
	if (ret < 0)
		goto err;

	ret = write(func->ep0_file, strings.data, strings.length);
	if (ret < 0)
		goto err;

	for (i = 0; i < func->ep_count; ++i) {
		sprintf(path, "%s/ep%d", func->ffs_path, i + 1);
		func->endpoints[i]->epfile = open(path, O_RDWR);
		if (func->endpoints[i]->epfile < 0) {
			ret = func->endpoints[i]->epfile;
			goto err_epfiles;
		}
	}

	/* Event loop plumbing: one io_context, one eventfd for AIO
	 * completions, one epoll set watching ep0 and the eventfd. */
	ret = io_setup(MAX_INFLIGHT, &func->aio_ctx);
	if (ret < 0)
		goto err_epfiles;

	func->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (func->event_fd < 0) {
		ret = -errno;
		goto err_aio;
	}

	func->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (func->epoll_fd < 0) {
		ret = -errno;
		goto err_eventfd;
	}

	eev.events = EPOLLIN;
	eev.data.u32 = TAG_EP0;
	if (epoll_ctl(func->epoll_fd, EPOLL_CTL_ADD, func->ep0_file, &eev) < 0) {
		ret = -errno;
		goto err_epoll;
	}
	eev.data.u32 = TAG_AIO;
	if (epoll_ctl(func->epoll_fd, EPOLL_CTL_ADD, func->event_fd, &eev) < 0) {
		ret = -errno;
		goto err_epoll;
	}

	ret = 0;
	goto out3;

err_epoll:
	close(func->epoll_fd);
	func->epoll_fd = -1;
err_eventfd:
	close(func->event_fd);
	func->event_fd = -1;
err_aio:
	io_destroy(func->aio_ctx);
	func->aio_ctx = 0;
err_epfiles:
	while (i)
		close(func->endpoints[--i]->epfile);
err:
	close(func->ep0_file);
	func->ep0_file = -1;
out3:
	free(path);
out2:
	__usbf_strings_free(&strings);
out:
	__usbf_descs_free(&descs);
	return ret;
}

void usbf_stop(struct usbf_function *func)
{
	int i;

	func->running = 0;

	if (func->epoll_fd >= 0) {
		close(func->epoll_fd);
		func->epoll_fd = -1;
	}
	if (func->event_fd >= 0) {
		close(func->event_fd);
		func->event_fd = -1;
	}
	if (func->aio_ctx) {
		io_destroy(func->aio_ctx);
		func->aio_ctx = 0;
	}

	for (i = 0; i < func->ep_count; ++i) {
		if (func->endpoints[i]->epfile >= 0) {
			close(func->endpoints[i]->epfile);
			func->endpoints[i]->epfile = -1;
		}
	}
	if (func->ep0_file >= 0) {
		close(func->ep0_file);
		func->ep0_file = -1;
	}
}

int usbf_submit(struct usbf_endpoint *ep, void *data, size_t length,
                usbf_completion_cb cb, void *user)
{
	struct usbf_function *func = ep->func;
	struct usbf_iocb *uio;
	struct iocb *iocbs[1];
	int ret;

	uio = func->free_iocb;
	if (!uio)
		return -EAGAIN;
	func->free_iocb = uio->next_free;

	uio->ep = ep;
	uio->data = data;
	uio->length = length;
	uio->complete = cb;
	uio->user = user;

	switch (ep->desc.direction) {
	case USBF_OUT:
		io_prep_pread(&uio->cb, ep->epfile, data, length, 0);
		break;
	case USBF_IN:
		io_prep_pwrite(&uio->cb, ep->epfile, data, length, 0);
		break;
	default:
		ret = -EINVAL;
		goto err;
	}
	io_set_eventfd(&uio->cb, func->event_fd);

	iocbs[0] = &uio->cb;
	ret = io_submit(func->aio_ctx, 1, iocbs);
	if (ret < 1) {
		/* io_submit returns 0 if no iocbs were accepted, negative
		 * errno on error. */
		if (ret == 0)
			ret = -EAGAIN;
		goto err;
	}
	return 0;

err:
	uio->next_free = func->free_iocb;
	func->free_iocb = uio;
	return ret;
}

static int drain_ep0_events(struct usbf_function *func)
{
	struct usb_functionfs_event event;
	struct usbf_setup_request setup;
	struct pollfd pfd = { .fd = func->ep0_file, .events = POLLIN };
	int ret;

	for (;;) {
		ret = poll(&pfd, 1, 0);
		if (ret < 0)
			return ret;
		if (ret == 0 || !(pfd.revents & POLLIN))
			return 0;

		ret = read(func->ep0_file, &event, sizeof(event));
		if (ret < 0)
			return ret;


		if (event.type == FUNCTIONFS_SETUP) {
			if (!func->desc.setup_handler) {
				ret = (event.u.setup.bRequestType & USB_DIR_IN)
					? read(func->ep0_file, NULL, 0)
					: write(func->ep0_file, NULL, 0);
				if (ret)
					return ret;
				continue;
			}
			setup.bRequestType = event.u.setup.bRequestType;
			setup.bRequest = event.u.setup.bRequest;
			setup.wValue = le16toh(event.u.setup.wValue);
			setup.wIndex = le16toh(event.u.setup.wIndex);
			setup.wLength = le16toh(event.u.setup.wLength);
			setup.function = func;
			ret = func->desc.setup_handler(&setup);
		} else if (func->desc.event_handler) {
			ret = func->desc.event_handler(event.type);
			if (ret)
				return ret;
		}
	}
}

static int drain_completions(struct usbf_function *func)
{
	struct io_event ev[16];
	struct timespec zero = { 0, 0 };
	struct usbf_iocb *uio;
	uint64_t wakeups;
	ssize_t r;
	int got, i;

	/* Consume the eventfd wakeup counter; its value is advisory, we drain
	 * io_getevents until it returns 0 regardless. */
	do {
		r = read(func->event_fd, &wakeups, sizeof(wakeups));
	} while (r < 0 && errno == EINTR);

	for (;;) {
		got = io_getevents(func->aio_ctx, 0, 16, ev, &zero);
		if (got <= 0)
			return got;

		for (i = 0; i < got; ++i) {
			uio = (struct usbf_iocb *)ev[i].obj;
			if (uio->complete)
				uio->complete(uio->ep, uio->data, uio->length,
				              ev[i].res, uio->user);
			uio->next_free = func->free_iocb;
			func->free_iocb = uio;
		}
	}
}

int usbf_get_fd(struct usbf_function *func)
{
	return func->epoll_fd;
}

int usbf_dispatch(struct usbf_function *func)
{
	struct epoll_event events[4];
	int n, i, ret;

	n = epoll_wait(func->epoll_fd, events, 4, 0);
	if (n < 0)
		return (errno == EINTR) ? 0 : -errno;

	for (i = 0; i < n; ++i) {
		if (events[i].data.u32 == TAG_EP0) {
			ret = drain_ep0_events(func);
			if (ret)
				return ret;
		} else if (events[i].data.u32 == TAG_AIO) {
			ret = drain_completions(func);
			if (ret < 0)
				return ret;
		}
	}
	return 0;
}

int usbf_run(struct usbf_function *func)
{
	struct epoll_event events[4];
	int n, i, ret;

	func->running = 1;
	while (func->running) {
		n = epoll_wait(func->epoll_fd, events, 4, -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		for (i = 0; i < n; ++i) {
			if (events[i].data.u32 == TAG_EP0) {
				ret = drain_ep0_events(func);
				if (ret)
					return ret;
			} else if (events[i].data.u32 == TAG_AIO) {
				ret = drain_completions(func);
				if (ret < 0)
					return ret;
			}
		}
	}
	return 0;
}

int usbf_setup_ack(const struct usbf_setup_request *setup)
{
	return (setup->bRequestType & USB_DIR_IN) ?
		write(setup->function->ep0_file, NULL, 0) :
		read(setup->function->ep0_file, NULL, 0);
}

int usbf_setup_response(const struct usbf_setup_request *setup,
	void *data, size_t length)
{
	return (setup->bRequestType & USB_DIR_IN) ?
		write(setup->function->ep0_file, data, length) :
		read(setup->function->ep0_file, data, length);
}

int usbf_setup_stall(const struct usbf_setup_request *setup)
{
	return (setup->bRequestType & USB_DIR_IN) ?
		read(setup->function->ep0_file, NULL, 0) :
		write(setup->function->ep0_file, NULL, 0);
}
