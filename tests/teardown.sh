#!/bin/bash
# Idempotent cleanup of the test gadget.  Useful when setup.sh died ungracefully
# (left configfs entries / FFS mount behind).
#
# Usage: ./teardown.sh

set +e

GADGET_NAME="${GADGET_NAME:-test}"
FFS_MOUNT="${FFS_MOUNT:-/dev/ffs/${GADGET_NAME}}"
GADGET_DIR="/sys/kernel/config/usb_gadget/${GADGET_NAME}"

if [ -e "${GADGET_DIR}/UDC" ]; then
	echo "" > "${GADGET_DIR}/UDC" 2>/dev/null
fi

# Kill any process still holding the FFS mount (e.g. a gadget binary that
# survived an unclean setup.sh exit).  Without this the umount below would
# fail with EBUSY.
if mountpoint -q "$FFS_MOUNT"; then
	fuser -k -TERM -m "$FFS_MOUNT" 2>/dev/null
	# Brief wait for the process to release file handles.
	for _ in 1 2 3 4 5; do
		fuser -m "$FFS_MOUNT" >/dev/null 2>&1 || break
		sleep 0.1
	done
	umount "$FFS_MOUNT"
fi

if [ -d "$FFS_MOUNT" ]; then
	rmdir "$FFS_MOUNT" 2>/dev/null
fi

if [ -d "$GADGET_DIR" ]; then
	rm -f "${GADGET_DIR}/configs/c.1/ffs.usb0"
	rmdir "${GADGET_DIR}/configs/c.1/strings/0x409" 2>/dev/null
	rmdir "${GADGET_DIR}/configs/c.1" 2>/dev/null
	rmdir "${GADGET_DIR}/functions/ffs.usb0" 2>/dev/null
	rmdir "${GADGET_DIR}/strings/0x409" 2>/dev/null
	rmdir "${GADGET_DIR}" 2>/dev/null
fi

echo "[teardown] done"
