/*
 * SPDX-License-Identifier: Unlicense
 */

#include <libusbf.h>

#include <stdio.h>
#include <string.h>
#include <errno.h>

static struct usbf_endpoint *ep_in, *ep_out;
static unsigned char buf[512];
static int enabled;

static void out_complete(struct usbf_endpoint *ep, void *data, size_t length,
                         ssize_t result, void *user);
static void in_complete(struct usbf_endpoint *ep, void *data, size_t length,
                        ssize_t result, void *user);

static void submit_out(void)
{
	int ret = usbf_submit(ep_out, buf, sizeof(buf), out_complete, NULL);
	if (ret < 0)
		fprintf(stderr, "submit OUT failed: %s\n", strerror(-ret));
}

static void submit_in(size_t n)
{
	int ret = usbf_submit(ep_in, buf, n, in_complete, NULL);
	if (ret < 0)
		fprintf(stderr, "submit IN failed: %s\n", strerror(-ret));
}

static void out_complete(struct usbf_endpoint *ep, void *data, size_t length,
                         ssize_t result, void *user)
{
	if (!enabled)
		return;
	if (result < 0) {
		fprintf(stderr, "OUT error: %s\n", strerror(-result));
		return;
	}
	submit_in(result);
}

static void in_complete(struct usbf_endpoint *ep, void *data, size_t length,
                        ssize_t result, void *user)
{
	if (!enabled)
		return;
	if (result < 0) {
		fprintf(stderr, "IN error: %s\n", strerror(-result));
		return;
	}
	submit_out();
}

static int event_handler(enum usbf_event_type event)
{
	static const char *const names[] = {
		[USBF_EVENT_BIND] = "BIND",
		[USBF_EVENT_UNBIND] = "UNBIND",
		[USBF_EVENT_ENABLE] = "ENABLE",
		[USBF_EVENT_DISABLE] = "DISABLE",
		[USBF_EVENT_SUSPEND] = "SUSPEND",
		[USBF_EVENT_RESUME] = "RESUME",
	};

	printf("EVENT: %s\n", names[event]);

	switch (event) {
	case USBF_EVENT_ENABLE:
		enabled = 1;
		submit_out();
		break;
	case USBF_EVENT_DISABLE:
		enabled = 0;
		break;
	default:
		break;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	struct usbf_function *my_func;
	int ret;

	struct usbf_function_descriptor f_desc = {
		.speed = USBF_SPEED_FS | USBF_SPEED_HS,
		.interface_class = USBF_CLASS_VENDOR_SPEC,
		.string = "TEST FUNCTION",
		.event_handler = event_handler,
		.setup_handler = NULL,
	};

	struct usbf_endpoint_descriptor ep_desc = {
		.type = USBF_BULK,
		.fs_maxpacketsize = 64,
		.hs_maxpacketsize = 512,
	};

	if (argc != 2) {
		fprintf(stderr, "FunctionFS mountpoint not specified!\n");
		return 1;
	}

	my_func = usbf_create_function(&f_desc, argv[1]);
	if (!my_func) {
		fprintf(stderr, "Function registration failed!\n");
		return 1;
	}

	ep_desc.direction = USBF_IN;
	ep_in = usbf_add_endpoint(my_func, &ep_desc);
	if (!ep_in) {
		fprintf(stderr, "Can not add in endpoint!\n");
		ret = 1;
		goto error;
	}

	ep_desc.direction = USBF_OUT;
	ep_out = usbf_add_endpoint(my_func, &ep_desc);
	if (!ep_out) {
		fprintf(stderr, "Can not add out endpoint!\n");
		ret = 1;
		goto error;
	}

	ret = usbf_start(my_func);
	if (ret < 0) {
		fprintf(stderr, "Function start failed!\n");
		ret = 1;
		goto error;
	}

	ret = usbf_run(my_func);
	if (ret < 0)
		fprintf(stderr, "Run failed: %s\n", strerror(-ret));

	usbf_stop(my_func);
	ret = 0;

error:
	usbf_delete_function(my_func);
	return ret;
}
