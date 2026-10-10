#!/vendor/bin/sh
# BRING-UP ONLY (2026-10-10). Stand-in for the NVIDIA EFI boot-control HAL, which we replaced with
# bootctrl.default. AndroidLauncher decrements RootfsRetrySlot<A|B> (NV UEFI variable) on every
# boot and switches to the other boot chain when it reaches 0; the HAL is supposed to restore it
# once the boot succeeded. Without this the board falls back to the empty chain B after 3 boots.
E=/sys/firmware/efi/efivars
G=781e084c-a330-417c-b678-38e696380cb9
grep -q " $E efivarfs" /proc/mounts || mount -t efivarfs efivarfs $E || exit 1

# Current OS chain (0 = A, 1 = B); value is the 4 bytes after the 4-byte attribute header.
cur=$(od -An -tu1 -j4 -N1 $E/BootChainOsCurrent-$G 2>/dev/null | tr -d ' ')
case "$cur" in
  1) slot=B ;;
  *) slot=A ;;
esac
F=$E/RootfsRetrySlot$slot-$G

T=/data/vendor/rootfs_retry.bin
# efivarfs needs attributes + data in a single write(): NV|BS|RT (0x07), then retry count 3.
printf '\007\000\000\000\003\000\000\000' > $T
chattr -i $F 2>/dev/null
dd if=$T of=$F bs=8 count=1 2>/dev/null
rm -f $T
log -t rootfs_retry "slot $slot retry count now: $(od -An -tu1 -j4 -N1 $F | tr -d ' ')"
