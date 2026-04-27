/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2014 Robert Baldyga
 */

#ifndef __LIBUSBF_H__
#define __LIBUSBF_H__

#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>

enum usbf_class {
	USBF_CLASS_PER_INTERFACE = 0,
	USBF_CLASS_AUDIO = 1,
	USBF_CLASS_COMM = 2,
	USBF_CLASS_HID = 3,
	USBF_CLASS_PHYSICAL = 5,
	USBF_CLASS_STILL_IMAGE = 6,
	USBF_CLASS_PRINTER = 7,
	USBF_CLASS_MASS_STORAGE = 8,
	USBF_CLASS_HUB = 9,
	USBF_CLASS_CDC_DATA = 0x0a,
	USBF_CLASS_CSCID = 0x0b,
	USBF_CLASS_CONTENT_SEC = 0x0d,
	USBF_CLASS_VIDEO = 0x0e,
	USBF_CLASS_WIRELESS_CONTROLLER = 0xe0,
	USBF_CLASS_MISC = 0xef,
	USBF_CLASS_APP_SPEC = 0xfe,
	USBF_CLASS_VENDOR_SPEC = 0xff,
	USBF_SUBCLASS_VENDOR_SPEC = 0xff,
};

enum usbf_flags {
	USBF_SPEED_FS = 0x01,
	USBF_SPEED_HS = 0x02,
	USBF_SPEED_SS = 0x04,
};

enum usbf_endpoint_type {
	USBF_ISOCHRONOUS = 0x01,
	USBF_BULK = 0x02,
	USBF_INTERRUPT = 0x03,
};

enum usbf_endpoint_direction {
	USBF_OUT = 0x00,
	USBF_IN = 0x80,
};

enum usbf_event_type {
	USBF_EVENT_BIND,
	USBF_EVENT_UNBIND,

	USBF_EVENT_ENABLE,
	USBF_EVENT_DISABLE,

	__USBF_EVENT_SETUP,

	USBF_EVENT_SUSPEND,
	USBF_EVENT_RESUME,
};

struct usbf_function;
struct usbf_endpoint;

struct usbf_setup_request {
	uint8_t bRequestType;
	uint8_t bRequest;
	uint16_t wValue;
	uint16_t wIndex;
	uint16_t wLength;

	struct usbf_function *function;
};

struct usbf_endpoint_descriptor {
	uint16_t fs_maxpacketsize;
	uint16_t hs_maxpacketsize;
	uint16_t ss_maxpacketsize;

	uint8_t fs_interval;
	uint8_t hs_interval;
	uint8_t ss_interval;

	enum usbf_endpoint_type type;

	enum usbf_endpoint_direction direction;
};

struct usbf_function_descriptor {
	uint32_t speed;
	uint8_t interface_class;
	char *string; /* TODO - USB strings handling */
	int (*event_handler)(enum usbf_event_type);
	int (*setup_handler)(const struct usbf_setup_request *);
};

/* Completion callback for usbf_submit. `result` follows kernel AIO semantics:
 * on success it is the number of bytes transferred (may be 0); on failure it
 * is a negative errno. `data` and `length` are the pointer and length originally
 * passed to usbf_submit. */
typedef void (*usbf_completion_cb)(struct usbf_endpoint *ep,
                                   void *data, size_t length,
                                   ssize_t result, void *user);

/* Lifecycle */
struct usbf_function *
usbf_create_function(struct usbf_function_descriptor *func, char *path);

void usbf_delete_function(struct usbf_function *func);

struct usbf_endpoint *usbf_add_endpoint(
	struct usbf_function *func, struct usbf_endpoint_descriptor *desc);

int usbf_start(struct usbf_function *func);

void usbf_stop(struct usbf_function *func);

/* Event loop integration.
 *
 * The library owns one internal epoll set that watches ep0 and the libaio
 * completion eventfd. Two usage modes:
 *
 *   Own loop:      just call usbf_run(func) - blocks until usbf_stop().
 *   External loop: add usbf_get_fd(func) to your own epoll/poll/select as
 *                  level-triggered POLLIN; call usbf_dispatch(func) when it
 *                  becomes readable.
 */
int usbf_get_fd(struct usbf_function *func);

int usbf_dispatch(struct usbf_function *func);

int usbf_run(struct usbf_function *func);

/* I/O submission. Returns 0 on success (callback will fire later), or a
 * negative errno on immediate failure (callback will NOT fire). */
int usbf_submit(struct usbf_endpoint *ep, void *data, size_t length,
                usbf_completion_cb cb, void *user);

/* ep0 setup-request helpers (called from setup_handler). */
int usbf_setup_ack(const struct usbf_setup_request *setup);

int usbf_setup_response(const struct usbf_setup_request *setup,
	void *data, size_t length);

/* Stall the current ep0 setup request. Returns 0 on success or a negative
 * errno on real failure; the kernel's stall-acknowledged signal (EL2HLT)
 * is treated as success and not surfaced to the caller. */
int usbf_setup_stall(const struct usbf_setup_request *setup);

#endif /* __LIBUSBF_H__ */
