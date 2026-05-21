#!/bin/sh
# TT #621 DEBUG VARIANT — single-function CDC-ACM gadget on K144.
# No composite (no ADB, no UAC1) so we can isolate whether Tab5's
# bulk-IN issue is composite-IAD related.  If round-trip works with
# this single-function gadget, the composite is the bug.  If not,
# the bug is in cdc_acm_host vs f_acm interaction at the protocol
# level.
#
# To revert: re-install /soc/scripts/usb-tinker.sh via install.sh.

GADGET_NAME=tinker_cdc
GADGET_ROOT=/etc/configfs/usb_gadget/${GADGET_NAME}
UDC_FILE=${GADGET_ROOT}/UDC

set -e

mkdir -p /etc/configfs
if ! mount | grep -q "/etc/configfs.*configfs"; then
   mount none /etc/configfs -t configfs
fi

# Tear down the composite (if it exists) so we own the UDC
for g in /etc/configfs/usb_gadget/usb_adb /etc/configfs/usb_gadget/tinker; do
   if [ -d "$g" ]; then
      echo "" > "$g/UDC" 2>/dev/null || true
      find "$g" -type l 2>/dev/null | xargs -I {} rm -f {}
   fi
done
killall adbd 2>/dev/null || true
umount /dev/usb-ffs/adb 2>/dev/null || true

mkdir -p "${GADGET_ROOT}"
cd "${GADGET_ROOT}"

echo 0x32c9 > idVendor
echo 0x2003 > idProduct
echo 0x0200 > bcdDevice
echo 0x0200 > bcdUSB

mkdir -p strings/0x409
echo "axera"             > strings/0x409/manufacturer
echo "ax620e-cdc-only"   > strings/0x409/product
echo "axera-ax620e"      > strings/0x409/serialnumber

mkdir -p functions/acm.usb0

mkdir -p configs/c.1/strings/0x409
echo "single-function CDC-ACM" > configs/c.1/strings/0x409/configuration
echo 250 > configs/c.1/MaxPower

ln -sf functions/acm.usb0 configs/c.1/

UDC_NAME=$(ls /sys/class/udc/ | head -1)
if [ -z "${UDC_NAME}" ]; then
   echo "[usb-tinker-cdc] no UDC found in /sys/class/udc/" >&2
   exit 1
fi
echo "${UDC_NAME}" > "${UDC_FILE}"

echo "[usb-tinker-cdc] single-function CDC-ACM bound on ${UDC_NAME}"
