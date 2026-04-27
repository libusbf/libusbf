/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2014-2026 Robert Baldyga
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
	const uint32_t speed_mask = USBF_SPEED_FS | USBF_SPEED_HS | USBF_SPEED_SS;
	struct usbf_function *func;
	int i;

	if (!desc || !path)
		return NULL;

	if (desc->speed & ~speed_mask)
		return NULL;
	if (!(desc->speed & speed_mask))
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
	func->interface_count = 0;
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
	int i, a, e, c;

	for (i = 0; i < func->interface_count; ++i) {
		struct usbf_interface *intf = func->interfaces[i];
		for (a = 0; a < intf->alt_count; ++a) {
			struct usbf_alt_setting *alt = intf->alts[a];
			for (c = 0; c < alt->class_desc_count; ++c) {
				free(alt->class_descs[c]->data);
				free(alt->class_descs[c]);
			}
			for (e = 0; e < alt->ep_count; ++e)
				free(alt->endpoints[e]);
			free(alt);
		}
		free(intf);
	}
	free(func->ffs_path);
	free(func);
}

struct usbf_interface *
usbf_add_interface(struct usbf_function *func,
                   struct usbf_interface_descriptor *desc)
{
	struct usbf_interface *intf;

	if (func->interface_count >= MAX_INTERFACES)
		return NULL;

	intf = malloc(sizeof(*intf));
	if (!intf)
		return NULL;

	memcpy(&intf->desc, desc, sizeof(*desc));
	intf->alt_count = 0;
	intf->intf_num = func->interface_count;
	intf->string_idx = 0;
	intf->func = func;

	func->interfaces[func->interface_count++] = intf;
	return intf;
}

struct usbf_alt_setting *usbf_add_alt_setting(struct usbf_interface *intf)
{
	struct usbf_alt_setting *alt;

	if (intf->alt_count >= MAX_ALT_SETTINGS)
		return NULL;

	alt = malloc(sizeof(*alt));
	if (!alt)
		return NULL;

	alt->ep_count = 0;
	alt->class_desc_count = 0;
	alt->alt_num = intf->alt_count;
	alt->intf = intf;

	intf->alts[intf->alt_count++] = alt;

	return alt;
}

int usbf_add_class_descriptor(struct usbf_alt_setting *alt,
                              const void *data, size_t length)
{
	struct usbf_class_descriptor *cd;

	/* USB descriptors carry their own length in the first byte; the blob
	 * passed here is one descriptor, so bLength must equal the byte
	 * count. Single-byte descriptors don't exist (every descriptor type
	 * has at least bLength + bDescriptorType), and bLength is uint8 so
	 * 255 bytes is the upper bound. */
	if (length < 2 || length > 255)
		return -EINVAL;
	if (((const uint8_t *)data)[0] != length)
		return -EINVAL;
	if (alt->class_desc_count >= MAX_CLASS_DESCS)
		return -ENOSPC;

	cd = malloc(sizeof(*cd));
	if (!cd)
		return -ENOMEM;
	cd->data = malloc(length);
	if (!cd->data) {
		free(cd);
		return -ENOMEM;
	}
	memcpy(cd->data, data, length);
	cd->length = length;

	alt->class_descs[alt->class_desc_count++] = cd;
	return 0;
}

/* Returns the total number of endpoints declared across all interfaces and
 * their alt-settings. Used to assign sequential ep numbers (FFS allocates one
 * epfile per declared endpoint, in the order they appear in the descriptors). */
static int total_ep_count(const struct usbf_function *func)
{
	int i, a, n = 0;
	for (i = 0; i < func->interface_count; ++i) {
		const struct usbf_interface *intf = func->interfaces[i];
		for (a = 0; a < intf->alt_count; ++a)
			n += intf->alts[a]->ep_count;
	}
	return n;
}

/* Validate one (speed, mps, interval) tuple against the USB spec rules
 * for the endpoint's transfer type. Lower 11 bits of mps are the
 * per-transaction size; bits 11..12 carry the high-bandwidth multiplier
 * for HS interrupt/isoc and must be 0 elsewhere. SS encodes burst/mult
 * in the SS companion descriptor instead, so its mps top bits must be 0
 * too. */
static int validate_endpoint_speed(enum usbf_endpoint_type type,
                                   uint16_t mps, uint8_t interval,
                                   uint32_t speed)
{
	uint16_t size = mps & 0x07ff;
	uint16_t mult = (mps >> 11) & 0x03;

	switch (type) {
	case USBF_BULK:
		if (mult != 0)
			return -EINVAL;
		if (speed == USBF_SPEED_FS) {
			if (size != 8 && size != 16 && size != 32 && size != 64)
				return -EINVAL;
		} else if (speed == USBF_SPEED_HS) {
			if (size != 512)
				return -EINVAL;
		} else { /* SS */
			if (size != 1024)
				return -EINVAL;
		}
		/* bInterval is unused for bulk on FS/SS; HS allows a NAK-rate
		 * hint in 0..255 which fits uint8_t natively, so nothing to
		 * check. */
		break;

	case USBF_INTERRUPT:
		if (size == 0)
			return -EINVAL;
		if (speed == USBF_SPEED_FS) {
			if (size > 64 || mult != 0)
				return -EINVAL;
			if (interval == 0)
				return -EINVAL;
			/* FS interrupt bInterval is in frames, 1..255 -
			 * naturally bounded by uint8_t. */
		} else if (speed == USBF_SPEED_HS) {
			if (size > 1024 || mult > 2)
				return -EINVAL;
			if (interval == 0 || interval > 16)
				return -EINVAL;
		} else { /* SS */
			if (size > 1024 || mult != 0)
				return -EINVAL;
			if (interval == 0 || interval > 16)
				return -EINVAL;
		}
		break;

	case USBF_ISOCHRONOUS:
		if (speed == USBF_SPEED_FS) {
			if (size > 1023 || mult != 0)
				return -EINVAL;
			if (interval == 0 || interval > 16)
				return -EINVAL;
		} else if (speed == USBF_SPEED_HS) {
			if (size > 1024 || mult > 2)
				return -EINVAL;
			if (interval == 0 || interval > 16)
				return -EINVAL;
		} else { /* SS */
			if (size > 1024 || mult != 0)
				return -EINVAL;
			if (interval == 0 || interval > 16)
				return -EINVAL;
		}
		break;
	}
	return 0;
}

/* SS endpoint companion descriptor sanity:
 *   bMaxBurst:           0..15 always.
 *   bmAttributes:        bulk encodes the max-streams exponent in bits[4:0]
 *                        (so bits[7:5] are reserved); isoc encodes Mult in
 *                        bits[1:0] with Mult <= 2 and bits[7:2] reserved;
 *                        interrupt has the whole byte reserved.
 *   wBytesPerInterval:   bulk MUST be 0 per spec; periodic eps reserve
 *                        bandwidth so any non-zero value is allowed. */
static int validate_ss_companion(enum usbf_endpoint_type type,
                                 const struct usbf_endpoint_descriptor *desc)
{
	if (desc->ss_max_burst > 15)
		return -EINVAL;

	switch (type) {
	case USBF_BULK:
		if (desc->ss_attributes & 0xe0)
			return -EINVAL;
		if (desc->ss_bytes_per_interval != 0)
			return -EINVAL;
		break;
	case USBF_INTERRUPT:
		if (desc->ss_attributes != 0)
			return -EINVAL;
		break;
	case USBF_ISOCHRONOUS:
		if (desc->ss_attributes & 0xfc)
			return -EINVAL;
		if ((desc->ss_attributes & 0x03) > 2)
			return -EINVAL;
		break;
	}
	return 0;
}

struct usbf_endpoint *usbf_add_endpoint(
	struct usbf_alt_setting *alt, struct usbf_endpoint_descriptor *desc)
{
	struct usbf_function *func = alt->intf->func;
	struct usbf_endpoint *ep;
	int ep_num;

	switch (desc->type) {
	case USBF_ISOCHRONOUS:
	case USBF_BULK:
	case USBF_INTERRUPT:
		break;
	default:
		return NULL;
	}

	if (desc->direction != USBF_IN && desc->direction != USBF_OUT)
		return NULL;

	if ((func->desc.speed & USBF_SPEED_FS) &&
	    validate_endpoint_speed(desc->type,
	                            desc->fs_maxpacketsize,
	                            desc->fs_interval, USBF_SPEED_FS) < 0)
		return NULL;
	if ((func->desc.speed & USBF_SPEED_HS) &&
	    validate_endpoint_speed(desc->type,
	                            desc->hs_maxpacketsize,
	                            desc->hs_interval, USBF_SPEED_HS) < 0)
		return NULL;
	if (func->desc.speed & USBF_SPEED_SS) {
		if (validate_endpoint_speed(desc->type,
		                            desc->ss_maxpacketsize,
		                            desc->ss_interval,
		                            USBF_SPEED_SS) < 0)
			return NULL;
		if (validate_ss_companion(desc->type, desc) < 0)
			return NULL;
	}

	if (alt->ep_count >= MAX_ENDPOINTS)
		return NULL;
	if (total_ep_count(func) >= MAX_ENDPOINTS)
		return NULL;

	ep = malloc(sizeof(*ep));
	if (!ep)
		return NULL;

	memcpy(&ep->desc, desc, sizeof(*desc));
	ep->func = func;
	ep->epfile = -1;

	alt->endpoints[alt->ep_count++] = ep;
	/* Sequential endpoint number across all interfaces+alts in declaration
	 * order. */
	ep_num = total_ep_count(func);
	ep->address = ep_num | ep->desc.direction;

	return ep;
}

/* Sum total alt-settings across all interfaces. */
static int total_alt_count(const struct usbf_function *func)
{
	int i, n = 0;
	for (i = 0; i < func->interface_count; ++i)
		n += func->interfaces[i]->alt_count;
	return n;
}

/* Walk every alt and accumulate class-descriptor counts and byte size. */
static void total_class_descs(const struct usbf_function *func,
                              int *out_count, size_t *out_bytes)
{
	int i, a, c, count = 0;
	size_t bytes = 0;
	for (i = 0; i < func->interface_count; ++i) {
		const struct usbf_interface *intf = func->interfaces[i];
		for (a = 0; a < intf->alt_count; ++a) {
			const struct usbf_alt_setting *alt = intf->alts[a];
			for (c = 0; c < alt->class_desc_count; ++c) {
				++count;
				bytes += alt->class_descs[c]->length;
			}
		}
	}
	*out_count = count;
	*out_bytes = bytes;
}

/* Walk interfaces and assign 1-based string indices to those that supplied a
 * string. Returns the number of strings to emit and the total NUL-included
 * byte length of all strings. Indices are dense (1, 2, ...) so the kernel's
 * needed_count check (max iInterface used) lines up with str_count. */
static void assign_string_indices(struct usbf_function *func,
                                  int *out_count, size_t *out_bytes)
{
	int i, idx = 1;
	size_t bytes = 0;

	for (i = 0; i < func->interface_count; ++i) {
		struct usbf_interface *intf = func->interfaces[i];
		if (intf->desc.string && intf->desc.string[0] != '\0') {
			intf->string_idx = idx++;
			bytes += strlen(intf->desc.string) + 1;
		} else {
			intf->string_idx = 0;
		}
	}
	*out_count = idx - 1;
	*out_bytes = bytes;
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
	struct usbf_interface *intf;
	struct usbf_alt_setting *alt;
	struct usbf_endpoint *ep;
	struct epoll_event eev;
	uint32_t speed;
	char *path;
	void *cur;
	int str_count;
	size_t str_bytes;
	int ret, i, a, e, k, opened;

	if (func->interface_count == 0)
		return -EINVAL;
	for (i = 0; i < func->interface_count; ++i)
		if (func->interfaces[i]->alt_count == 0)
			return -EINVAL;

	{
		int alts = total_alt_count(func);
		int eps = total_ep_count(func);
		int n_class;
		size_t class_bytes;
		size_t base_size;
		int base_count;
		uint32_t s;
		int idx = 0;

		total_class_descs(func, &n_class, &class_bytes);

		/* All speeds share the same interface descriptors, class
		 * descriptors, and endpoint descriptors. SS additionally
		 * appends a 6-byte SS companion after each endpoint. */
		base_size = alts * sizeof(struct usb_interface_descriptor) +
			class_bytes +
			eps * sizeof(struct usb_endpoint_descriptor_no_audio);
		base_count = alts + n_class + eps;

		descs.speeds = !!(func->flags & USBF_SPEED_FS) +
			!!(func->flags & USBF_SPEED_HS) +
			!!(func->flags & USBF_SPEED_SS);

		for (s = USBF_SPEED_FS; s <= USBF_SPEED_SS; s <<= 1) {
			if (!(func->flags & s))
				continue;
			descs.per_speed_size[idx] = base_size;
			descs.per_speed_count[idx] = base_count;
			if (s == USBF_SPEED_SS) {
				descs.per_speed_size[idx] += eps *
					sizeof(struct usb_ss_ep_comp_descriptor);
				descs.per_speed_count[idx] += eps;
			}
			++idx;
		}
	}

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
		*count_ptr = htole32(descs.per_speed_count[i]);
	}

	/* String indices must be assigned before descriptor emission so each
	 * interface descriptor can reference its iInterface slot. */
	assign_string_indices(func, &str_count, &str_bytes);

	speed = 1;
	for (k = 0; k < descs.speeds; ++k) {
		while (!(speed & func->flags))
			speed <<= 1;
		cur = __usbf_descs_access_speed_block(&descs, k);
		for (i = 0; i < func->interface_count; ++i) {
			intf = func->interfaces[i];
			for (a = 0; a < intf->alt_count; ++a) {
				int c;
				alt = intf->alts[a];
				intf_desc = cur;
				intf_desc->bLength = sizeof(*intf_desc);
				intf_desc->bDescriptorType = USB_DT_INTERFACE;
				intf_desc->bInterfaceNumber = intf->intf_num;
				intf_desc->bAlternateSetting = alt->alt_num;
				intf_desc->bNumEndpoints = alt->ep_count;
				intf_desc->bInterfaceClass =
					intf->desc.interface_class;
				intf_desc->bInterfaceSubClass =
					intf->desc.interface_subclass;
				intf_desc->bInterfaceProtocol =
					intf->desc.interface_protocol;
				intf_desc->iInterface = intf->string_idx;
				cur += sizeof(*intf_desc);
				/* Class-specific descriptors (e.g. HID, CCID,
				 * DFU functional) sit between the interface
				 * descriptor and its endpoint descriptors. */
				for (c = 0; c < alt->class_desc_count; ++c) {
					struct usbf_class_descriptor *cd =
						alt->class_descs[c];
					memcpy(cur, cd->data, cd->length);
					cur += cd->length;
				}
				for (e = 0; e < alt->ep_count; ++e) {
					ep = alt->endpoints[e];
					ep_desc = cur;
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
					cur += sizeof(*ep_desc);
					/* SS companion follows every endpoint
					 * descriptor in the SS speed block. */
					if (speed == USBF_SPEED_SS) {
						struct usb_ss_ep_comp_descriptor *comp = cur;
						comp->bLength = sizeof(*comp);
						comp->bDescriptorType = USB_DT_SS_ENDPOINT_COMP;
						comp->bMaxBurst = ep->desc.ss_max_burst;
						comp->bmAttributes = ep->desc.ss_attributes;
						comp->wBytesPerInterval =
							htole16(ep->desc.ss_bytes_per_interval);
						cur += sizeof(*comp);
					}
				}
			}
		}
		speed <<= 1;
	}

	/* Strings table: one LangID (en-US 0x0409) with str_count strings
	 * (one per interface that supplied a non-empty string). When no
	 * interface declared a string, str_count == 0 and the body is empty;
	 * FFS accepts that as long as no descriptor references a string. */
	if (str_count > 0)
		strings.body_length = sizeof(__le16) + str_bytes;
	else
		strings.body_length = 0;
	strings.str_count = str_count;
	ret = __usbf_strings_alloc(&strings);
	if (ret < 0)
		goto out;

	strings_header = __usbf_strings_access_header(&strings);
	strings_header->magic = htole32(FUNCTIONFS_STRINGS_MAGIC);
	strings_header->length = htole32(strings.length);
	strings_header->str_count = htole32(str_count);
	strings_header->lang_count = htole32(str_count > 0 ? 1 : 0);

	if (str_count > 0) {
		char *body = __usbf_strings_body(&strings);
		*(__le16 *)body = htole16(0x0409);
		body += sizeof(__le16);
		for (i = 0; i < func->interface_count; ++i) {
			struct usbf_interface *p = func->interfaces[i];
			if (p->string_idx == 0)
				continue;
			strcpy(body, p->desc.string);
			body += strlen(p->desc.string) + 1;
		}
	}

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

	opened = 0;
	for (i = 0; i < func->interface_count; ++i) {
		intf = func->interfaces[i];
		for (a = 0; a < intf->alt_count; ++a) {
			alt = intf->alts[a];
			for (e = 0; e < alt->ep_count; ++e) {
				sprintf(path, "%s/ep%d", func->ffs_path, opened + 1);
				alt->endpoints[e]->epfile = open(path, O_RDWR);
				if (alt->endpoints[e]->epfile < 0) {
					ret = alt->endpoints[e]->epfile;
					goto err_epfiles;
				}
				++opened;
			}
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
	for (i = 0; i < func->interface_count && opened > 0; ++i) {
		intf = func->interfaces[i];
		for (a = 0; a < intf->alt_count && opened > 0; ++a) {
			alt = intf->alts[a];
			for (e = 0; e < alt->ep_count && opened > 0; ++e) {
				close(alt->endpoints[e]->epfile);
				alt->endpoints[e]->epfile = -1;
				--opened;
			}
		}
	}
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
	int i, a, e;

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

	for (i = 0; i < func->interface_count; ++i) {
		struct usbf_interface *intf = func->interfaces[i];
		for (a = 0; a < intf->alt_count; ++a) {
			struct usbf_alt_setting *alt = intf->alts[a];
			for (e = 0; e < alt->ep_count; ++e) {
				if (alt->endpoints[e]->epfile >= 0) {
					close(alt->endpoints[e]->epfile);
					alt->endpoints[e]->epfile = -1;
				}
			}
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
		do {
			ret = poll(&pfd, 1, 0);
		} while (ret < 0 && errno == EINTR);
		if (ret < 0)
			return -errno;
		if (ret == 0 || !(pfd.revents & POLLIN))
			return 0;

		do {
			ret = read(func->ep0_file, &event, sizeof(event));
		} while (ret < 0 && errno == EINTR);
		if (ret < 0)
			return -errno;


		if (event.type == FUNCTIONFS_SETUP) {
			if (!func->desc.setup_handler) {
				/* Stall via wrong-direction zero-length read or
				 * write. FFS signals stall success with -EL2HLT
				 * via errno (the kernel ack); treat it as
				 * success. Any other negative is fatal. */
				if (event.u.setup.bRequestType & USB_DIR_IN)
					ret = read(func->ep0_file, NULL, 0);
				else
					ret = write(func->ep0_file, NULL, 0);
				if (ret < 0 && errno != EL2HLT)
					return -errno;
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

ssize_t usbf_setup_response(const struct usbf_setup_request *setup,
	void *data, size_t length)
{
	return (setup->bRequestType & USB_DIR_IN) ?
		write(setup->function->ep0_file, data, length) :
		read(setup->function->ep0_file, data, length);
}

int usbf_setup_stall(const struct usbf_setup_request *setup)
{
	ssize_t r;

	/* Wrong-direction zero-length transfer triggers a stall: read on an
	 * IN setup or write on an OUT setup. FFS reports stall-success via
	 * errno = EL2HLT (the kernel ack — see __ffs_ep0_stall in f_fs.c),
	 * so swallow that and return 0; any other negative is fatal. */
	if (setup->bRequestType & USB_DIR_IN)
		r = read(setup->function->ep0_file, NULL, 0);
	else
		r = write(setup->function->ep0_file, NULL, 0);
	if (r < 0 && errno != EL2HLT)
		return -errno;
	return 0;
}
