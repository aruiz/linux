#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Boot linux.efi with a GPT-partitioned disk and verify partition detection.
#
set -e

EFI=${1:-arch/eml/boot/linux.efi}
OVMF=/usr/share/edk2/ovmf/OVMF.stateless.fd
TIMEOUT=${TIMEOUT:-20}

if [ ! -f "$EFI" ]; then
	echo "error: $EFI not found (run: make ARCH=eml)" >&2
	exit 1
fi

if [ ! -f "$OVMF" ]; then
	echo "error: OVMF firmware not found at $OVMF" >&2
	exit 1
fi

if ! command -v sgdisk >/dev/null 2>&1; then
	echo "error: sgdisk not found (install gdisk)" >&2
	exit 1
fi

ESP=$(mktemp /tmp/eml-esp-XXXXXX.img)
DISK=$(mktemp /tmp/eml-gpt-XXXXXX.img)
trap 'rm -f "$ESP" "$DISK"' EXIT

# ESP with EFI binary
dd if=/dev/zero of="$ESP" bs=1M count=64 2>/dev/null
mkfs.vfat -F 32 "$ESP" >/dev/null 2>&1
mmd -i "$ESP" ::EFI ::EFI/BOOT
mcopy -i "$ESP" "$EFI" ::EFI/BOOT/BOOTX64.EFI

# Second disk with GPT + one partition
dd if=/dev/zero of="$DISK" bs=1M count=32 2>/dev/null
sgdisk -n 1:2048:+8M -t 1:8300 -c 1:"testpart" "$DISK" >/dev/null 2>&1

OUTPUT=$(timeout "$TIMEOUT" qemu-system-x86_64 \
	-bios "$OVMF" \
	-drive file="$ESP",format=raw \
	-drive file="$DISK",format=raw,if=ide \
	-nographic -no-reboot \
	-m 256M \
	${EXTRA_QEMU_ARGS:-} \
	2>&1) || true

PASS=0
FAIL=0

check() {
	if echo "$OUTPUT" | grep -q "$1"; then
		echo "PASS: $1"
		PASS=$((PASS + 1))
	else
		echo "FAIL: expected '$1' in output" >&2
		FAIL=$((FAIL + 1))
	fi
}

check "Hello from EFI Mode Linux"
check "EML: timer event created"
check "EML: found.*Block I/O"
check "efiblk0"

echo "---"
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
