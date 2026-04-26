# Standalone Makefile for the test gadgets.  Links against libusbf.a built
# in the parent libusbf tree (run libusbf's own build first).

LIBUSBF_DIR ?= ../..
LIBUSBF_A   := $(LIBUSBF_DIR)/src/.libs/libusbf.a

CFLAGS  ?= -g -O2 -Wall
CFLAGS  += -I$(LIBUSBF_DIR)/include
LIBAIO  := -Wl,--whole-archive,-l:libaio.a,--no-whole-archive

GADGETS := bulk_loopback control_loopback interrupt_loopback mixed_endpoints \
           altsetting_loopback two_interfaces hid_minimal ss_bulk_loopback \
           halt_loopback cancel_loopback iad_minimal all_ctrl_recip \
           speed_query

all: $(GADGETS)

%: %.c $(LIBUSBF_A)
	$(CC) $(CFLAGS) -o $@ $< $(LIBUSBF_A) $(LIBAIO)

$(LIBUSBF_A):
	@echo "libusbf.a missing; build libusbf first (./autogen.sh && ./configure && make in $(LIBUSBF_DIR))" >&2
	@false

clean:
	rm -f $(GADGETS) *.o

.PHONY: all clean
