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

#ifndef __LIBUSBF_DESCS_H__
#define __LIBUSBF_DESCS_H__

#include "libusbf_private.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

struct __usbf_descs {
	int speeds;
	int endpoints;
	size_t length;
	void *data;
};

struct __usbf_strings {
	size_t str_length;
	size_t length;
	void *data;
};

inline int __usbf_descs_alloc(struct __usbf_descs *descs)
{
	descs->length =
		sizeof(struct usb_functionfs_descs_head_v2) + descs->speeds *
		(sizeof(struct usb_interface_descriptor) + descs->endpoints *
		 sizeof(struct usb_endpoint_descriptor_no_audio) +
		 sizeof(__le32));
	descs->data = malloc(descs->length);
	return descs->data ? 0 : -ENOMEM;
}

inline void __usbf_descs_free(struct __usbf_descs *descs)
{
	free(descs->data);
}

inline struct usb_functionfs_descs_head_v2 *
	__usbf_descs_access_header(struct __usbf_descs *descs)
{
	return descs->data;
}

inline __le32 *__usbf_descs_access_count(struct __usbf_descs *descs, int spd_idx)
{
	return descs->data + sizeof(struct usb_functionfs_descs_head_v2) +
		spd_idx * sizeof(__le32);
}

inline struct usb_interface_descriptor *__usbf_descs_access_interface(
	struct __usbf_descs *descs, int spd_idx)
{
	return descs->data + sizeof(struct usb_functionfs_descs_head_v2) +
		descs->speeds * sizeof(__le32) + spd_idx *
		(sizeof(struct usb_interface_descriptor) + descs->endpoints *
		 sizeof(struct usb_endpoint_descriptor_no_audio));
}

inline struct usb_endpoint_descriptor_no_audio *__usbf_descs_access_endpoint(
	struct __usbf_descs *descs, int spd_idx, int ep_idx)
{
	return descs->data + sizeof(struct usb_functionfs_descs_head_v2) +
		descs->speeds * sizeof(__le32) + spd_idx *
		(sizeof(struct usb_interface_descriptor) + descs->endpoints *
		 sizeof(struct usb_endpoint_descriptor_no_audio)) +
		sizeof(struct usb_interface_descriptor) +
		sizeof(struct usb_endpoint_descriptor_no_audio) * ep_idx;
}

inline int __usbf_strings_alloc(struct __usbf_strings *strings)
{
	strings->length =
		sizeof(struct usb_functionfs_strings_head) +
		sizeof(__le16) + strings->str_length+1;
	strings->data = malloc(strings->length);
	return strings->data ? 0 : -ENOMEM;
}

inline void __usbf_strings_free(struct __usbf_strings *strings)
{
	free(strings->data);
}

inline struct usb_functionfs_strings_head *
	__usbf_strings_access_header(struct __usbf_strings *strings)
{
	return strings->data;
}

inline void __usbf_strings_set_code(struct __usbf_strings *strings, __le16 code)
{
	__le16 *ptr = strings->data +
		sizeof(struct usb_functionfs_strings_head);
	*ptr = code;
}

inline int  __usbf_strings_set_string(struct __usbf_strings *strings, char *string)
{
	if (strlen(string) != strings->str_length)
		return -EINVAL;
	char *ptr = strings->data + sizeof(__le16) +
		sizeof(struct usb_functionfs_strings_head);
	strcpy(ptr, string);

	return 0;
}

#endif /* __LIBUSBF_DESCS_H__ */
