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

struct usbf_function *
usbf_create_function(struct usbf_function_descriptor *desc, char *path)
{
	if (desc->speed & ~(0x07))
		return NULL;
	if (!(desc->speed & 0x07))
		return NULL;

	struct usbf_function *func = malloc(sizeof(*func));
	if (!func)
		return NULL;

	func->ffs_path = malloc(strlen(path));
	if (!func->ffs_path) {
		free(func);
		return NULL;
	}

	strcpy(func->ffs_path, path);
	memcpy(&func->desc, desc, sizeof(*desc));

	func->flags = func->desc.speed;
	func->ep_count = 0;

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
		*count_ptr = htole32(descs.endpoints+1);
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
				ep_desc->bInterval = ep->desc.fs_interval;
				break;
			case USBF_SPEED_SS:
				ep_desc->wMaxPacketSize =
					htole16(ep->desc.ss_maxpacketsize);
				ep_desc->bInterval = ep->desc.fs_interval;
				break;
			}
		}
		speed <<= 1;
	}

	/* FIXME - strings generation process is very 'simplified' for now */
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
	path = malloc(strlen(func->ffs_path)+6);
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
		sprintf(path, "%s/ep%d", func->ffs_path, i+1);
		func->endpoints[i]->epfile = open(path, O_RDWR);
		if (func->endpoints[i]->epfile < 0) {
			ret = func->endpoints[i]->epfile;
			goto err_epfiles;
		}
	}

	goto out3;

err_epfiles:
	while (i)
		close(func->endpoints[--i]->epfile);
err:
	close(func->ep0_file);
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

	for (i = 0; i < func->ep_count; ++i)
		close(func->endpoints[i]->epfile);
	close(func->ep0_file);
}

int usbf_transfer(struct usbf_endpoint *ep, void *data, size_t length)
{
	/* TODO - check if (lenght <= maxpacketsize) for current speed */

	switch (ep->desc.direction) {
	case USBF_OUT:
		return read(ep->epfile, data, length);
	case USBF_IN:
		return write(ep->epfile, data, length);
	default:
		return -EINVAL;
	}
}

int usbf_handle_events(struct usbf_function *func)
{
	struct usb_functionfs_event event;
	struct usbf_setup_request setup;
	int ret;

	struct pollfd pfds[1];
	pfds[0].fd = func->ep0_file;
	pfds[0].events = POLLIN;

	while ((ret = poll(pfds, 1, 0)) && (pfds[0].revents & POLLIN)) {
		ret = read(func->ep0_file, &event, sizeof(event));
		if (ret < 0)
			return ret;
		if (event.type == FUNCTIONFS_SETUP) {
			if (!func->desc.setup_handler) {
				if (event.u.setup.bRequestType & USB_DIR_IN) {
					ret = read(func->ep0_file, NULL, 0);
					if (ret)
						return ret;
				} else {
					ret = write(func->ep0_file, NULL, 0);
					if (ret)
						return ret;
				}
				continue;
			}
			setup.bRequestType = event.u.setup.bRequestType;
			setup.bRequest = event.u.setup.bRequest;
			setup.wValue = le16toh(event.u.setup.wValue);
			setup.wIndex = le16toh(event.u.setup.wIndex);
			setup.wLength = le16toh(event.u.setup.wLength);
			setup.function = func;
			ret = func->desc.setup_handler(&setup);
		} else {
			if (func->desc.event_handler)
				ret = func->desc.event_handler(event.type);
		}
		if (ret)
			return ret;
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
