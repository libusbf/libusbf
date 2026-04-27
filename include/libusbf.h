/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2014-2026 Robert Baldyga
 */

/*
 * libusbf - declarative USB function library on top of FunctionFS.
 *
 * The user describes the USB function as a tree of objects:
 *
 *     usbf_function
 *       (optional usbf_iad_descriptor)
 *       usbf_interface
 *         usbf_alt_setting
 *           (optional class-specific descriptor blobs)
 *           usbf_endpoint
 *           ...
 *         ...
 *       ...
 *
 * libusbf synthesizes the USB descriptor blob and strings table from that
 * tree, writes them to FunctionFS' ep0, opens the data endpoint files, and
 * dispatches events plus AIO completions through an event loop.
 *
 * Typical usage:
 *
 *     usbf_create_function()      construct the function
 *     usbf_add_interface()        \
 *     usbf_add_alt_setting()      |  describe topology
 *     usbf_add_endpoint()         /
 *     usbf_set_iad()              optional, before usbf_start
 *     usbf_start()                push descriptors, open endpoint files
 *     usbf_run()                  drive the event loop until stopped
 *     usbf_stop()                 unwind on shutdown
 *     usbf_delete_function()      free the function tree
 *
 * Within callbacks (event_handler / setup_handler / completion callbacks)
 * the library is single-threaded: every callback fires from the same
 * thread that drives the event loop, so user code does not need locking
 * around libusbf state.
 */

#ifndef __LIBUSBF_H__
#define __LIBUSBF_H__

#include <stdint.h>
#include <sys/types.h>

/* USB-IF assigned interface class codes. The set tracks the interface-class
 * column from https://www.usb.org/defined-class-codes; users can also pass a
 * raw byte (cast to enum usbf_class) for codes not enumerated here. */
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
};

/* Interface subclass codes. Most subclasses are class-specific (e.g.
 * audio control vs. audio streaming under USBF_CLASS_AUDIO), so libusbf
 * does not enumerate them; the one universal value is the vendor-specific
 * placeholder that pairs with USBF_CLASS_VENDOR_SPEC. */
enum usbf_subclass {
	USBF_SUBCLASS_VENDOR_SPEC = 0xff,
};

/* Bitmask of negotiable bus speeds. OR these into
 * usbf_function_descriptor.speed to declare which speeds the function
 * supports; FunctionFS rejects the descriptor blob if the gadget is bound
 * to a UDC whose speed isn't represented here. The same values are returned
 * by usbf_get_speed() to identify the actually-negotiated speed at
 * runtime. */
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

/* Endpoint transfer type. Values map directly to the bmAttributes[1:0]
 * field of the USB endpoint descriptor (control = 0 is reserved for ep0
 * and never appears here). */
enum usbf_endpoint_type {
	USBF_ISOCHRONOUS = 0x01,
	USBF_BULK = 0x02,
	USBF_INTERRUPT = 0x03,
};

/* Endpoint direction, as encoded in bit 7 of bEndpointAddress. */
enum usbf_endpoint_direction {
	USBF_OUT = 0x00,  /* host -> gadget */
	USBF_IN = 0x80,   /* gadget -> host */
};

/* Event types passed to usbf_function_descriptor.event_handler. The values
 * match FunctionFS' event numbering so libusbf can forward them verbatim.
 *
 * BIND / UNBIND fire when the gadget is bound to / unbound from a USB
 * configuration at the configfs/composite layer.
 *
 * ENABLE / DISABLE fire when the host transitions the configuration in or
 * out of "configured" state (SET_CONFIGURATION). All in-flight AIO
 * transfers are completed (with a negative result) by the kernel on
 * DISABLE; the gadget normally re-arms its receive buffers on ENABLE.
 *
 * SUSPEND / RESUME fire on USB bus suspend/resume (3+ ms of idle).
 */
enum usbf_event_type {
	USBF_EVENT_BIND,
	USBF_EVENT_UNBIND,

	USBF_EVENT_ENABLE,
	USBF_EVENT_DISABLE,

	/* Placeholder so subsequent values keep their FunctionFS-matching
	 * numbers. SETUP requests are intercepted by libusbf and routed to
	 * usbf_function_descriptor.setup_handler instead, so this value is
	 * never delivered to event_handler. */
	__USBF_EVENT_SETUP,

	USBF_EVENT_SUSPEND,
	USBF_EVENT_RESUME,
};

struct usbf_function;
struct usbf_interface;
struct usbf_alt_setting;
struct usbf_endpoint;

/* Setup packet delivered to setup_handler. The five USB-spec fields carry
 * native (host) byte order; libusbf converts from little-endian on the wire
 * before invoking the handler. `function` is the function this setup
 * targets - handlers can pass it to other libusbf entry points
 * (usbf_find_endpoint, usbf_cancel_all, etc.) without keeping a side table. */
struct usbf_setup_request {
	uint8_t bRequestType;
	uint8_t bRequest;
	uint16_t wValue;
	uint16_t wIndex;
	uint16_t wLength;

	struct usbf_function *function;
};

/* Per-endpoint shape, replicated per-speed because a single endpoint emits
 * one descriptor per declared speed and they may differ (e.g. bulk uses 64
 * mps at FS but 512 at HS). Only fields for speeds the function actually
 * declares (usbf_function_descriptor.speed) are read; the rest may stay 0.
 *
 * <speed>_maxpacketsize:
 *     wMaxPacketSize, in bytes. Bulk: 8/16/32/64 at FS, 512 at HS, 1024 at
 *     SS. Interrupt: up to 64 at FS, up to 1024 at HS, up to 1024 at SS.
 *     Isochronous: up to 1023 at FS, up to 1024 at HS (with mult), up to
 *     1024 at SS.
 *
 * <speed>_interval:
 *     bInterval. Bulk endpoints ignore it. Interrupt at FS uses frames
 *     (1..255 ms). Interrupt/isoc at HS or SS use 2^(bInterval-1)
 *     microframes (125 us units); valid range 1..16. */
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

	/* Transfer type (bulk / interrupt / isoc). Control endpoints (ep0)
	 * are owned by FunctionFS and not declared here. */
	enum usbf_endpoint_type type;

	/* USBF_IN (gadget -> host) or USBF_OUT (host -> gadget). */
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

/* Per-function configuration, passed to usbf_create_function. The
 * descriptor is copied by value, so the caller does not need to keep it
 * around after the call.
 *
 * speed:
 *     Bitmask of usbf_flags identifying which negotiated speeds the
 *     function supports. Must be non-zero and only contain
 *     USBF_SPEED_FS / HS / SS bits.
 *
 * flags:
 *     Bitmask of usbf_function_flags. May be 0.
 *
 * event_handler:
 *     Optional. Called for non-SETUP ep0 events (BIND, UNBIND, ENABLE,
 *     DISABLE, SUSPEND, RESUME). A non-zero return causes usbf_run() /
 *     usbf_dispatch() to abort with that value. May be NULL if the
 *     function does not need to react to lifecycle transitions.
 *
 * setup_handler:
 *     Optional. Called for every SETUP packet that FunctionFS routes to
 *     this function (Interface- and Endpoint-recipient by default; add
 *     Device/Other recipients via USBF_ALL_CTRL_RECIP). The handler must
 *     finalize the request with usbf_setup_ack / usbf_setup_response /
 *     usbf_setup_stall before returning. Its return value is currently
 *     ignored by libusbf - error reporting goes through the setup helper
 *     return codes. May be NULL, in which case libusbf stalls every
 *     SETUP that reaches the function. */
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

/* Lifecycle.
 *
 * Construct an empty function from `desc` rooted at the FunctionFS mount
 * `path` (e.g. "/dev/ffs/myfunc"; the FunctionFS instance must already be
 * mounted). The returned handle owns no kernel resources yet - that
 * happens at usbf_start. Returns NULL on validation failure or
 * allocation error.
 *
 * libusbf copies `desc` and stores `path` as a null-terminated string;
 * neither pointer needs to outlive this call. */
struct usbf_function *
usbf_create_function(struct usbf_function_descriptor *desc, char *path);

/* Tear down the function tree built by usbf_create_function and freeing
 * every interface, alt-setting, endpoint and class-descriptor blob
 * underneath it. Safe to call after usbf_stop, or directly on a
 * never-started function. */
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

/* Add an alternate setting to an interface. bAlternateSetting is assigned
 * in the order alt-settings are added on that interface - first call
 * returns alt 0, second returns 1, and so on. Every interface must have
 * at least one alt-setting (alt 0). Returns NULL on allocation failure
 * or when the per-interface alt-setting limit is reached. */
struct usbf_alt_setting *usbf_add_alt_setting(struct usbf_interface *intf);

/* Add a data endpoint to an alt-setting. Endpoint numbers are assigned
 * sequentially across the entire function in declaration order - the
 * first endpoint added (across any interface/alt) becomes endpoint 1,
 * the second endpoint 2, and so on; bEndpointAddress combines that
 * number with the direction bit from `desc->direction`. Returns NULL on
 * allocation failure or when the per-alt endpoint limit is reached.
 *
 * libusbf copies `desc` by value; the caller does not need to keep it
 * around. */
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

/* Activate the function: synthesize the FFS V2 descriptor blob and the
 * strings table from the topology built so far, write them to ep0, open
 * every data endpoint file, and stand up the internal event loop
 * (epoll set + libaio context + completion eventfd). After a successful
 * return the function is ready to be bound to a UDC at the
 * configfs/composite layer; the host-visible enumeration kicks off as
 * soon as that binding completes. The function must already have at
 * least one interface with at least one alt-setting. Returns 0 on
 * success or a negative errno. */
int usbf_start(struct usbf_function *func);

/* Reverse usbf_start: wake any usbf_run() that's currently blocked in
 * epoll_wait so it sees the cleared running flag and returns, then tear
 * down the event loop, close every endpoint file plus ep0, and release
 * the libaio context. Any in-flight AIO is canceled by the kernel during
 * io_destroy; if the user wants their completion callbacks to fire first
 * they should call usbf_cancel_all and drive the event loop until those
 * callbacks drain before calling usbf_stop. The function tree itself
 * remains intact and can be usbf_start()ed again, or freed via
 * usbf_delete_function.
 *
 * Threading: safe to call from a setup_handler / event_handler /
 * completion callback (the run loop notices on its way back to
 * epoll_wait). For a cross-thread stop, the caller must ensure
 * usbf_run() / usbf_dispatch() on the loop thread has returned before
 * the resource teardown here closes the fds out from under it; the
 * eventfd write done above wakes the loop, but synchronizing the actual
 * exit is the caller's responsibility. */
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

/* Return the level-triggered file descriptor that signals "libusbf has
 * work pending" - readiness for POLLIN means the caller should invoke
 * usbf_dispatch. The fd is owned by libusbf and stays valid between
 * usbf_start and usbf_stop; do not close it. */
int usbf_get_fd(struct usbf_function *func);

/* Drain any pending ep0 events and AIO completions, dispatching them to
 * the user's event_handler / setup_handler / completion callbacks. Safe
 * to call when no work is pending (returns 0 quickly). Returns 0 on
 * success or a negative errno; if event_handler returned non-zero, that
 * value is propagated here. Intended for callers driving their own
 * event loop in conjunction with usbf_get_fd. */
int usbf_dispatch(struct usbf_function *func);

/* Run libusbf's built-in event loop. Blocks until usbf_stop() clears the
 * running flag and writes to the loop's wakeup eventfd, at which point
 * the next epoll_wait returns and the loop exits. Returns 0 on clean
 * shutdown or a negative errno on a fatal I/O error; an event_handler
 * returning non-zero also exits the loop with that value. See usbf_stop
 * for the threading contract. */
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

/* ep0 setup-request helpers (called from setup_handler). Each helper
 * finalizes the current setup transaction; setup_handler must call
 * exactly one of usbf_setup_ack / usbf_setup_response / usbf_setup_stall
 * before returning. */

/* Complete a no-data-stage setup request (the host expects a status
 * stage with no payload). Returns 0 on success or a negative errno. */
int usbf_setup_ack(const struct usbf_setup_request *setup);

/* Complete a setup request with a data stage. For IN setups (host reads),
 * `data`/`length` is the payload to send; the kernel forwards up to
 * `setup->wLength` bytes and short-completes the rest. For OUT setups
 * (host writes), `data` is the buffer to receive into and `length` is
 * its capacity (must be >= wLength). Returns the number of bytes
 * transferred on success or a negative errno on failure. */
ssize_t usbf_setup_response(const struct usbf_setup_request *setup,
	void *data, size_t length);

/* Stall the current ep0 setup request. Returns 0 on success or a negative
 * errno on real failure; the kernel's stall-acknowledged signal (EL2HLT)
 * is treated as success and not surfaced to the caller. */
int usbf_setup_stall(const struct usbf_setup_request *setup);

#endif /* __LIBUSBF_H__ */
