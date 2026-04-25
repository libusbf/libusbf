/*
 * SPDX-License-Identifier: Unlicense
 */

#include <libusbf.h>

int event_handler(enum usbf_event_type event)
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

	return 0;
}

int main(int argc, char *argv[])
{
	struct usbf_function *my_func;
	struct usbf_endpoint *ep_in, *ep_out;
	unsigned char buf[512];
	int ret, i;

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

	for (i = 0; i < 42; ++i) {
		usbf_handle_events(my_func);
		ret = usbf_transfer(ep_out, buf, sizeof(buf));
		if (ret < 0)
			fprintf(stderr, "Transfer error (out)!\n");
		ret = usbf_transfer(ep_in, buf, sizeof(buf));
		if (ret < 0)
			fprintf(stderr, "Transfer error (in)!\n");
	}

	usbf_stop(my_func);

error:
	usbf_delete_function(my_func);

	return ret;
}
