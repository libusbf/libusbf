#!/bin/bash
# Bring up a USB gadget backed by FunctionFS, run a test gadget program, bind
# it to a UDC, and tear everything down on exit (Ctrl-C or signal).
#
# Usage: ./setup.sh [--keep] <gadget-binary> [args...]
#
# The gadget binary is invoked with the FFS mount path as its first argument
# (additional args follow).  When this script exits, the UDC is unbound, the
# gadget process killed, FFS unmounted, and configfs entries removed.
#
# Override defaults via env vars: GADGET_NAME, FFS_MOUNT, UDC, VID, PID.

set -e

GADGET_NAME="${GADGET_NAME:-test}"
FFS_MOUNT="${FFS_MOUNT:-/dev/ffs/${GADGET_NAME}}"
UDC="${UDC:-dummy_udc.0}"
VID="${VID:-0x1d6b}"
PID="${PID:-0x0104}"

CONFIGFS=/sys/kernel/config/usb_gadget
GADGET_DIR="${CONFIGFS}/${GADGET_NAME}"
KEEP=0

if [ "$1" = "--keep" ]; then
	KEEP=1
	shift
fi

if [ -z "$1" ]; then
	echo "usage: $0 [--keep] <gadget-binary> [args...]" >&2
	exit 1
fi

if [ -z "$UDC" ]; then
	echo "no UDC available under /sys/class/udc/" >&2
	exit 1
fi

GADGET_BIN="$1"
shift
GADGET_ARGS=("$@")
GADGET_PID=

cleanup() {
	set +e
	if [ -n "$GADGET_PID" ]; then
		kill "$GADGET_PID" 2>/dev/null || true
		wait "$GADGET_PID" 2>/dev/null || true
	fi
	if [ -e "${GADGET_DIR}/UDC" ]; then
		echo "" > "${GADGET_DIR}/UDC" 2>/dev/null || true
	fi
	if mountpoint -q "$FFS_MOUNT"; then
		umount "$FFS_MOUNT" || true
	fi
	if [ -d "$FFS_MOUNT" ]; then
		rmdir "$FFS_MOUNT" 2>/dev/null || true
	fi
	if [ -d "$GADGET_DIR" ]; then
		rm -f "${GADGET_DIR}/configs/c.1/ffs.usb0"
		rmdir "${GADGET_DIR}/configs/c.1/strings/0x409" 2>/dev/null || true
		rmdir "${GADGET_DIR}/configs/c.1" 2>/dev/null || true
		rmdir "${GADGET_DIR}/functions/ffs.usb0" 2>/dev/null || true
		rmdir "${GADGET_DIR}/strings/0x409" 2>/dev/null || true
		rmdir "${GADGET_DIR}" 2>/dev/null || true
	fi
}

if [ "$KEEP" = "0" ]; then
	trap cleanup EXIT INT TERM
fi

# Ensure libcomposite is available (auto-loads on first usb_gadget access).
modprobe libcomposite 2>/dev/null || true

# Configfs structure.
mkdir -p "${GADGET_DIR}"
echo "$VID" > "${GADGET_DIR}/idVendor"
echo "$PID" > "${GADGET_DIR}/idProduct"

mkdir -p "${GADGET_DIR}/strings/0x409"
echo "libusbf-tests" > "${GADGET_DIR}/strings/0x409/manufacturer"
echo "${GADGET_NAME}" > "${GADGET_DIR}/strings/0x409/product"
echo "0001" > "${GADGET_DIR}/strings/0x409/serialnumber"

mkdir -p "${GADGET_DIR}/configs/c.1/strings/0x409"
echo "Default" > "${GADGET_DIR}/configs/c.1/strings/0x409/configuration"
echo 120 > "${GADGET_DIR}/configs/c.1/MaxPower"

mkdir -p "${GADGET_DIR}/functions/ffs.usb0"
# Use -n to avoid dereferencing an existing symlink at the destination
# (a leftover from a prior unclean run would otherwise cause ln -sf to
# create the new link *inside* the old one).
ln -snf "${GADGET_DIR}/functions/ffs.usb0" "${GADGET_DIR}/configs/c.1/ffs.usb0"

# Mount FunctionFS.
mkdir -p "$FFS_MOUNT"
if ! mountpoint -q "$FFS_MOUNT"; then
	mount -t functionfs usb0 "$FFS_MOUNT"
fi

echo "[setup] gadget=${GADGET_DIR} ffs=${FFS_MOUNT} udc=${UDC}"
echo "[setup] starting gadget: ${GADGET_BIN} ${FFS_MOUNT} ${GADGET_ARGS[*]}"

# Start the gadget program.  It must write descriptors and strings to ep0
# before we bind the UDC, so we wait briefly for ep0 to be opened.
"$GADGET_BIN" "$FFS_MOUNT" "${GADGET_ARGS[@]}" &
GADGET_PID=$!

# Wait for the gadget to finish initializing.  For gadgets with data
# endpoints this is signaled by ep1 appearing under the FFS mount (FFS only
# creates ep files after the descriptor blob is parsed).  Control-only
# gadgets never create ep1; we accept "process still alive after the grace
# period" as a fallback signal - a real descriptor-write failure would have
# caused the process to exit.
for _ in $(seq 1 20); do
	if [ -e "${FFS_MOUNT}/ep1" ]; then
		break
	fi
	if ! kill -0 "$GADGET_PID" 2>/dev/null; then
		echo "[setup] gadget process exited before writing descriptors" >&2
		exit 1
	fi
	sleep 0.1
done

if ! kill -0 "$GADGET_PID" 2>/dev/null; then
	echo "[setup] gadget process died during startup" >&2
	exit 1
fi

echo "[setup] gadget initialized, binding UDC ${UDC}"
echo "$UDC" > "${GADGET_DIR}/UDC"

echo "[setup] gadget bound; PID=${GADGET_PID}.  Press Ctrl-C to tear down."
wait "$GADGET_PID"
