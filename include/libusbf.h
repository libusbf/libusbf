/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2014-2026 Robert Baldyga
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

enum usbf_function_flags {
	/* Forward control requests with non-Interface and non-Endpoint
	 * recipients (Device, Other) to setup_handler. Without this flag,
	 * FunctionFS only routes Interface- and Endpoint-recipient setup
	 * requests to the gadget; everything else is handled by the kernel
	 * composite layer. Set this when the gadget owns vendor/Device or
	 * standard/Device requests (e.g. some HID GET_REPORT layouts,
	 * vendor-specific device-recipient requests). The wIndex passed
	 * to setup_handler is the raw value from the bus when the request
	 * is non-Interface and non-Endpoint. */
	USBF_ALL_CTRL_RECIP = 0x01,
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
struct usbf_interface;
struct usbf_alt_setting;
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

	/* SuperSpeed endpoint companion fields. Used only when the function
	 * declares USBF_SPEED_SS; ignored otherwise. The default-zero values
	 * describe the simplest valid SS endpoint: no burst, no streams (bulk),
	 * single packet per service interval (periodic).
	 *
	 *   ss_max_burst:           bMaxBurst, 0..15. 0 means "single packet
	 *                           per interval".
	 *   ss_attributes:          raw bmAttributes byte. Bulk: bits[4:0] are
	 *                           the max-streams exponent (0 = no streams,
	 *                           N = 2^N streams). Isoc: bits[1:0] are Mult
	 *                           (0..2). Interrupt: reserved (must be 0).
	 *   ss_bytes_per_interval:  wBytesPerInterval. Periodic eps reserve
	 *                           bus bandwidth based on this value (typically
	 *                           mps * (mult+1) * (max_burst+1)). Bulk
	 *                           endpoints MUST set this to 0 per spec. */
	uint8_t ss_max_burst;
	uint8_t ss_attributes;
	uint16_t ss_bytes_per_interval;

	enum usbf_endpoint_type type;

	enum usbf_endpoint_direction direction;
};

/* Class/subclass/protocol live on the USB interface descriptor, which is
 * emitted once per (interface, alt-setting) pair, so the spec technically
 * permits them to vary across alts of the same interface.  In practice this
 * is essentially never done (UAC bandwidth profiles, UVC streaming, HID
 * variants all keep class/subclass/protocol pinned per interface and only
 * vary endpoint shape), so libusbf attaches them at the interface level and
 * applies the same triple to every alt of that interface.  If a per-alt
 * override is ever needed, it can be added as an optional field on the
 * alt-setting layer without breaking this API. */
struct usbf_interface_descriptor {
	uint8_t interface_class;
	uint8_t interface_subclass;
	uint8_t interface_protocol;
	/* en-US interface name; NULL leaves iInterface = 0. */
	char *string;
};

struct usbf_function_descriptor {
	uint32_t speed;
	uint32_t flags;
	int (*event_handler)(enum usbf_event_type);
	int (*setup_handler)(const struct usbf_setup_request *);
};

/* Optional Interface Association Descriptor for the whole function. When
 * set, libusbf emits one IAD before the first interface descriptor with
 * bFirstInterface = 0 and bInterfaceCount = number of interfaces declared,
 * marking every interface of this function as a single logical group on
 * the host. Required by some composite-class stacks (CDC ACM, UVC, UAC2
 * with terminals). The libusbf "function" abstraction maps 1:1 to one
 * IAD; multi-IAD layouts are expressed as separate FunctionFS functions
 * composed at the configfs/composite layer. */
struct usbf_iad_descriptor {
	uint8_t function_class;
	uint8_t function_subclass;
	uint8_t function_protocol;
	/* en-US iFunction name; NULL or "" leaves iFunction = 0. */
	char *string;
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

/* Attach an Interface Association Descriptor to the function. Must be called
 * before usbf_start. Calling more than once replaces the previous IAD.
 * libusbf stores the descriptor by value (copying the struct); the iFunction
 * `string` pointer is borrowed and must remain valid until usbf_start
 * returns. Returns 0 on success or a negative errno. */
int usbf_set_iad(struct usbf_function *func,
                 struct usbf_iad_descriptor *desc);

/* Add an interface to the function. bInterfaceNumber is assigned in the order
 * interfaces are added: first call returns interface 0, second returns 1, and
 * so on. A function must have at least one interface (with at least one
 * alt-setting) before usbf_start. */
struct usbf_interface *
usbf_add_interface(struct usbf_function *func,
                   struct usbf_interface_descriptor *desc);

/* Add an alternate setting to an interface. bAlternateSetting is assigned in
 * the order alt-settings are added on that interface. */
struct usbf_alt_setting *usbf_add_alt_setting(struct usbf_interface *intf);

struct usbf_endpoint *usbf_add_endpoint(
	struct usbf_alt_setting *alt, struct usbf_endpoint_descriptor *desc);

/* Append one class-specific descriptor (e.g. HID, CCID, DFU functional) to
 * an alt-setting. The bytes are emitted between the alt's interface
 * descriptor and its endpoint descriptors, in the order added; `data[0]`
 * is bLength and must equal `length`. Call once per descriptor.
 *
 * libusbf copies the bytes immediately, so `data` need not outlive this
 * call. Returns 0 on success or a negative errno on failure. */
int usbf_add_class_descriptor(struct usbf_alt_setting *alt,
                              const void *data, size_t length);

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

/* Cancel every in-flight submit on `ep`. The kernel completes each canceled
 * iocb synchronously; libusbf invokes its completion callback with the
 * kernel's cancellation result (typically a negative errno such as
 * -ECONNRESET) before freeing the iocb slot. Returns the number of submits
 * actually canceled. Must be called from the same thread that drives the
 * event loop. */
int usbf_cancel(struct usbf_endpoint *ep);

/* Cancel every in-flight submit on the function, regardless of endpoint.
 * Same callback / return semantics as usbf_cancel; intended for clean
 * shutdown paths where the user wants every outstanding buffer accounted
 * for before tearing the function down. */
int usbf_cancel_all(struct usbf_function *func);

/* Stall a data endpoint from the gadget side. The host's next transfer on
 * this endpoint receives a STALL handshake until the halt is cleared
 * (either by the gadget via usbf_clear_halt or by the host via a standard
 * ClearFeature(HALT) request). Pending AIO submits stay queued and resume
 * once the halt is cleared. Returns 0 on success or a negative errno. */
int usbf_halt(struct usbf_endpoint *ep);

/* Clear a halted endpoint's stall state. Idempotent: calling on an
 * unhalted endpoint is a no-op. Returns 0 on success or a negative errno. */
int usbf_clear_halt(struct usbf_endpoint *ep);

/* Look up an endpoint by its logical number, as delivered in wIndex to
 * setup_handler for endpoint-recipient setup requests (FFS strips the
 * direction bit during reverse-mapping, so wIndex is the bare number 1..15).
 * Returns the matching usbf_endpoint, or NULL if no endpoint with that
 * number is declared on the function. Useful for routing ClearFeature(HALT)
 * and similar per-endpoint requests without keeping a manual mapping. */
struct usbf_endpoint *
usbf_find_endpoint(struct usbf_function *func, uint8_t number);

/* Query the negotiated bus speed of the function. Returns one of
 * USBF_SPEED_FS, USBF_SPEED_HS, or USBF_SPEED_SS. Returns 0 when the
 * function is not enumerated, the UDC's speed is undetermined, or more
 * than one UDC on the system is currently negotiated (libusbf cannot
 * tell which one belongs to this function from the FFS path alone).
 * Reads current_speed from sysfs on each call so it tracks reconnects
 * and reset transitions. */
int usbf_get_speed(struct usbf_function *func);

/* ep0 setup-request helpers (called from setup_handler). */
int usbf_setup_ack(const struct usbf_setup_request *setup);

ssize_t usbf_setup_response(const struct usbf_setup_request *setup,
	void *data, size_t length);

/* Stall the current ep0 setup request. Returns 0 on success or a negative
 * errno on real failure; the kernel's stall-acknowledged signal (EL2HLT)
 * is treated as success and not surfaced to the caller. */
int usbf_setup_stall(const struct usbf_setup_request *setup);

#endif /* __LIBUSBF_H__ */
