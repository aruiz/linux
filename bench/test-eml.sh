#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Boot linux.efi in QEMU+OVMF and verify kernel boot messages on serial.
#
set -e

EFI=${1:-arch/eml/boot/linux.efi}
OVMF=/usr/share/edk2/ovmf/OVMF.stateless.fd
TIMEOUT=${TIMEOUT:-15}

if [ ! -f "$EFI" ]; then
	echo "error: $EFI not found (run: make ARCH=eml)" >&2
	exit 1
fi

if [ ! -f "$OVMF" ]; then
	echo "error: OVMF firmware not found at $OVMF" >&2
	exit 1
fi

IMG=$(mktemp /tmp/eml-esp-XXXXXX.img)
trap 'rm -f "$IMG"' EXIT

dd if=/dev/zero of="$IMG" bs=1M count=64 2>/dev/null
mkfs.vfat -F 32 "$IMG" >/dev/null 2>&1
mmd -i "$IMG" ::EFI
mmd -i "$IMG" ::EFI/BOOT
mcopy -i "$IMG" "$EFI" ::EFI/BOOT/BOOTX64.EFI

OUTPUT=$(timeout "$TIMEOUT" qemu-system-x86_64 \
	-bios "$OVMF" \
	-drive file="$IMG",format=raw \
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
check "Linux version"
check "printk: legacy bootconsole \[eml0\] enabled"
check "Zone ranges:"
check "SLUB:"
check "EML: timer event created"
check "efiblk0"

echo "---"
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
