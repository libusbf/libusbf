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

#ifndef __LIBUSBF_H__
#define __LIBUSBF_H__

#include <stdio.h>
#include <stdint.h>

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

struct usbf_endpoint;

struct usbf_function_descriptor {
	uint32_t speed;
	uint8_t interface_class;
	char *string; /* TODO - USB strings handling */
	int (*event_handler)(enum usbf_event_type);
	int (*setup_handler)(const struct usbf_setup_request *);
};

struct usbf_function;

struct usbf_function *
usbf_create_function(struct usbf_function_descriptor *func, char *path);

void usbf_delete_function(struct usbf_function *func);


struct usbf_endpoint *usbf_add_endpoint(
	struct usbf_function *func, struct usbf_endpoint_descriptor *desc);


int usbf_start(struct usbf_function *func);

void usbf_stop(struct usbf_function *func);


int usbf_transfer(struct usbf_endpoint *ep, void *data, size_t length);

int usbf_handle_events(struct usbf_function *func);

int usbf_setup_ack(const struct usbf_setup_request *setup);

int usbf_setup_response(const struct usbf_setup_request *setup,
	void *data, size_t length);

int usbf_setup_stall(const struct usbf_setup_request *setup);

#endif /* __LIBUSBF_H__ */
