/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2014-2026 Robert Baldyga
 */

#ifndef __LIBUSBF_DESCS_H__
#define __LIBUSBF_DESCS_H__

#include "libusbf_private.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* Layout of the FunctionFS V2 descriptor blob this helper allocates:
 *
 *   [head_v2]                           <-- magic/length/flags
 *   [count_le32] x speeds               <-- descriptor count per speed
 *   [per-speed block] x speeds          <-- variable-sized, see below
 *
 * Each per-speed block is a sequence of (intf_desc + ep_desc x N) groups,
 * one group per (interface, alt-setting) pair, in declaration order. The
 * total number of descriptors per speed (count_le32) is alt_count_total +
 * total_eps, where alt_count_total sums alts across all interfaces.
 */

struct __usbf_descs {
	int speeds;
	int alt_count_total;
	int total_eps;
	size_t length;
	void *data;
};

/* Layout of the FunctionFS strings blob:
 *
 *   [strings_head]                  <-- magic/length/str_count/lang_count
 *   [body]                          <-- str_count == 0 ? empty
 *
 * For str_count > 0 the caller fills the body sequentially per LangID:
 *
 *   [__le16 LangID]
 *   [NUL-terminated string] x str_count
 *
 * libusbf only ever writes one LangID (en-US 0x0409); body_length covers
 * exactly that block.
 */

struct __usbf_strings {
	int str_count;
	size_t body_length;
	size_t length;
	void *data;
};

inline size_t __usbf_descs_speed_block_size(const struct __usbf_descs *descs)
{
	return descs->alt_count_total *
		sizeof(struct usb_interface_descriptor) +
		descs->total_eps *
		sizeof(struct usb_endpoint_descriptor_no_audio);
}

inline int __usbf_descs_alloc(struct __usbf_descs *descs)
{
	descs->length =
		sizeof(struct usb_functionfs_descs_head_v2) +
		descs->speeds * sizeof(__le32) +
		descs->speeds * __usbf_descs_speed_block_size(descs);
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

/* Returns a byte pointer to the start of spd_idx's per-speed block. The
 * caller walks the block sequentially, writing one interface descriptor
 * followed by its endpoint descriptors per (interface, alt) pair. */
inline void *__usbf_descs_access_speed_block(struct __usbf_descs *descs,
		int spd_idx)
{
	return descs->data + sizeof(struct usb_functionfs_descs_head_v2) +
		descs->speeds * sizeof(__le32) +
		spd_idx * __usbf_descs_speed_block_size(descs);
}

inline int __usbf_strings_alloc(struct __usbf_strings *strings)
{
	strings->length =
		sizeof(struct usb_functionfs_strings_head) +
		strings->body_length;
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

inline void *__usbf_strings_body(struct __usbf_strings *strings)
{
	return strings->data + sizeof(struct usb_functionfs_strings_head);
}

#endif /* __LIBUSBF_DESCS_H__ */
