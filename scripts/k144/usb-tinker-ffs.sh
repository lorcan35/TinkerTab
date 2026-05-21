#!/bin/sh
# TT #621 escape hatch — composite gadget with a new userspace-owned
# bulk function (ffs.control) that replaces broken acm.usb0 for the
# Tab5→K144 control plane.
#
# Functions:
#   1. ffs.adb       — original Axera ADB (recovery shell, untouched)
#   2. ffs.control   — Tinker Control bulk pair, bridged to TCP 10001
#                      by /usr/local/bin/tinker-ffs-control
#   3. acm.usb0      — kept for fallback / A-B testing; will be removed
#                      once ffs.control is proven
#   4. uac1.usb0     — UAC1 audio (16 kHz mono) — kept for now, will be
#                      replaced by ffs.audio_in/out in a later step
#
# Lives at /soc/scripts/usb-tinker-ffs.sh on K144.

GADGET_NAME=tinker
GADGET_ROOT=/etc/configfs/usb_gadget/${GADGET_NAME}
UDC_FILE=${GADGET_ROOT}/UDC

VID=0x32c9
PID=0x2003
MFR="axera"
PRODUCT="ax620e-tinker"
SERIAL="axera-ax620e"

UAC_SAMPLE_RATE=16000

set -e

mount_configfs() {
   if ! mount | grep -q "${GADGET_ROOT%/usb_gadget/*}.*configfs"; then
      mkdir -p "${GADGET_ROOT%/usb_gadget/*}"
      mount none "${GADGET_ROOT%/usb_gadget/*}" -t configfs
   fi
}

stop_gadget() {
   if [ -e "${UDC_FILE}" ]; then
      echo "" > "${UDC_FILE}" 2>/dev/null || true
   fi
   find "${GADGET_ROOT}" -type l 2>/dev/null | xargs -I {} rm -f {}
   umount /dev/usb-ffs/adb 2>/dev/null || true
   umount /dev/usb-ffs/control 2>/dev/null || true
   umount /dev/usb-ffs/video 2>/dev/null || true
   killall adbd 2>/dev/null || true
   killall tinker-ffs-control 2>/dev/null || true
   killall tinker-ffs-video 2>/dev/null || true
}

start_gadget() {
   mount_configfs

   mkdir -p "${GADGET_ROOT}"
   cd "${GADGET_ROOT}"

   echo "${VID}" > idVendor
   echo "${PID}" > idProduct
   echo 0x0419 > bcdDevice
   echo 0x0200 > bcdUSB

   mkdir -p strings/0x409
   echo "${MFR}"     > strings/0x409/manufacturer
   echo "${PRODUCT}" > strings/0x409/product
   echo "${SERIAL}"  > strings/0x409/serialnumber

   # ---- Function 1: ADB (ffs) ----
   mkdir -p functions/ffs.adb
   mkdir -p /dev/usb-ffs/adb -m 0770
   if ! mountpoint -q /dev/usb-ffs/adb; then
      mount -t functionfs adb /dev/usb-ffs/adb
   fi

   # ---- Function 2: Tinker Control (ffs) — TT #621 W5 ----
   # Userspace daemon owns the bulk endpoints; bridges to TCP 10001.
   mkdir -p functions/ffs.control
   mkdir -p /dev/usb-ffs/control -m 0770
   if ! mountpoint -q /dev/usb-ffs/control; then
      mount -t functionfs control /dev/usb-ffs/control
   fi

   # ---- Function 2.5: Tinker Video (ffs) — TT #621 W6 ----
   # Dedicated bulk pair for YOLO/vision payloads.  Same TCP 10001
   # target, separate socket, so it doesn't contend with control.
   mkdir -p functions/ffs.video
   mkdir -p /dev/usb-ffs/video -m 0770
   if ! mountpoint -q /dev/usb-ffs/video; then
      mount -t functionfs video /dev/usb-ffs/video
   fi

   # ---- Function 3: CDC-ACM (kept for fallback) ----
   mkdir -p functions/acm.usb0

   # ---- Function 4: UAC1 ----
   mkdir -p functions/uac1.usb0
   echo "${UAC_SAMPLE_RATE}" > functions/uac1.usb0/c_srate
   echo "${UAC_SAMPLE_RATE}" > functions/uac1.usb0/p_srate
   echo 2 > functions/uac1.usb0/c_ssize
   echo 2 > functions/uac1.usb0/p_ssize
   echo 1 > functions/uac1.usb0/c_chmask
   echo 1 > functions/uac1.usb0/p_chmask

   # ---- Configuration ----
   mkdir -p configs/c.1/strings/0x409
   echo "tinker composite (adb+ctl+acm+uac1)" > configs/c.1/strings/0x409/configuration
   echo 250 > configs/c.1/MaxPower

   ln -sf functions/ffs.adb      configs/c.1/
   ln -sf functions/ffs.control  configs/c.1/
   ln -sf functions/ffs.video    configs/c.1/
   ln -sf functions/acm.usb0     configs/c.1/
   ln -sf functions/uac1.usb0    configs/c.1/

   # ---- Spawn userspace daemons before binding UDC ----
   # functionfs gadgets only enumerate cleanly once their userspace
   # daemons have written descriptors to ep0.
   if ! pgrep -x adbd >/dev/null 2>&1; then
      adbd &
      sleep 1
   fi
   if ! pgrep -x tinker-ffs-control >/dev/null 2>&1; then
      /usr/local/bin/tinker-ffs-control &
      sleep 1
   fi
   if ! pgrep -x tinker-ffs-video >/dev/null 2>&1; then
      /usr/local/bin/tinker-ffs-video &
      sleep 1
   fi

   # ---- Bind to UDC ----
   UDC_NAME=$(ls /sys/class/udc/ | head -1)
   if [ -z "${UDC_NAME}" ]; then
      echo "[usb-tinker-ffs] no UDC found in /sys/class/udc/" >&2
      exit 1
   fi
   echo "${UDC_NAME}" > "${UDC_FILE}"

   echo "[usb-tinker-ffs] bound: ${GADGET_NAME} → ${UDC_NAME}"
   echo "[usb-tinker-ffs]   ffs.adb     (vendor class, adbd)"
   echo "[usb-tinker-ffs]   ffs.control (vendor class, TCP 10001 bridge)"
   echo "[usb-tinker-ffs]   ffs.video   (vendor class, TCP 10001 bridge — yolo/vision)"
   echo "[usb-tinker-ffs]   acm.usb0    (fallback /dev/ttyGS0)"
   echo "[usb-tinker-ffs]   uac1.usb0   (${UAC_SAMPLE_RATE} Hz mono)"
}

case "${1:-}" in
   start)   start_gadget ;;
   stop)    stop_gadget; echo "[usb-tinker-ffs] gadget torn down" ;;
   restart) stop_gadget; sleep 1; start_gadget ;;
   *)       echo "usage: $0 {start|stop|restart}" >&2; exit 1 ;;
esac
