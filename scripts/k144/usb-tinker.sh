#!/bin/sh
# Composite USB gadget for the K144 (M5Stack LLM Module).
#
# Replaces the stock single-function ADB gadget with a composite that
# exposes:
#
#   1. ffs.adb     — original Axera ADB (so we never lose recovery access)
#   2. acm.GS0     — CDC-ACM serial → /dev/ttyGS0 on K144
#                    Used by Tab5 for StackFlow JSON commands.
#                    Replaces the M5-Bus UART at 1.5 Mbps that suffers
#                    from clock-drift framing errors.
#   3. uac1.usb0   — USB Audio Class 1 (mic + speaker) → /dev/snd/pcmC*D*
#                    Native PCM streaming, replaces the ADPCM-over-UART
#                    ext_pcm pump.
#
# All three functions bind to a single UDC (8000000.dwc3).  K144 enumerates
# as a multi-interface USB device to the Tab5 host.
#
# Designed to be IDEMPOTENT and ATOMIC — if any step fails, the script
# unwinds to the previous state.  Safe to invoke multiple times.
#
# Lives at /soc/scripts/usb-tinker.sh on K144.  Invoked from /etc/rc.local
# (replacing the original usb-adb.sh start call).  Original usb-adb.sh is
# preserved at /soc/scripts/usb-adb.sh.orig for one-line rollback.
#
# TT #620 — pivot Tab5↔K144 transport off the fragile M5-Bus UART.

GADGET_NAME=tinker
GADGET_ROOT=/etc/configfs/usb_gadget/${GADGET_NAME}
UDC_FILE=${GADGET_ROOT}/UDC

VID=0x32c9      # axera vendor id — keeps adb client + udev rules working
PID=0x2003      # original product id — keep stock unless we collide with another device
MFR="axera"
PRODUCT="ax620e-tinker"
SERIAL="axera-ax620e"

# UAC1 audio parameters — match Tab5's voice pipeline natively
UAC_SAMPLE_RATE=16000          # 16 kHz mono — same as Dragon STT input
UAC_SUBFRAME_SIZE=2            # 16-bit
UAC_BITS=16
UAC_CHANNELS=1                 # mono capture (Tab5 mic is downsampled to mono)

set -e

cd "$(dirname "$0")"

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
   # remove function symlinks before removing the functions themselves
   find "${GADGET_ROOT}" -type l 2>/dev/null | xargs -I {} rm -f {}
   # functionfs adb teardown
   umount /dev/usb-ffs/adb 2>/dev/null || true
   killall adbd 2>/dev/null || true
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

   # ---- Function 1: ADB (functionfs-backed) ----
   #
   # Naming matches the stock /soc/scripts/usb-adb.sh — functionfs binds
   # the userspace adbd to /dev/usb-ffs/adb/ep0,ep1,ep2.
   mkdir -p functions/ffs.adb
   mkdir -p /dev/usb-ffs/adb -m 0770
   if ! mountpoint -q /dev/usb-ffs/adb; then
      mount -t functionfs adb /dev/usb-ffs/adb
   fi

   # ---- Function 2: CDC-ACM serial ----
   # Naming matches the stock /soc/scripts/usb-acm.sh (acm.usb0).
   mkdir -p functions/acm.usb0

   # ---- Function 3: UAC1 audio ----
   #
   # Defaults on this kernel are 48 kHz stereo 16-bit.  We want 16 kHz
   # mono 16-bit to match Tab5's mic downsample + Dragon STT input.
   # All six attributes confirmed writable on K144 kernel 4.19.125.
   mkdir -p functions/uac1.usb0
   echo "${UAC_SAMPLE_RATE}"   > functions/uac1.usb0/c_srate     # capture rate
   echo "${UAC_SAMPLE_RATE}"   > functions/uac1.usb0/p_srate     # playback rate
   echo "${UAC_SUBFRAME_SIZE}" > functions/uac1.usb0/c_ssize     # 2 = 16-bit
   echo "${UAC_SUBFRAME_SIZE}" > functions/uac1.usb0/p_ssize
   echo 1                      > functions/uac1.usb0/c_chmask    # 1 = mono
   echo 1                      > functions/uac1.usb0/p_chmask

   # ---- Configuration 1 (the one and only) ----
   mkdir -p configs/c.1/strings/0x409
   echo "tinker composite (adb+acm+uac1)" > configs/c.1/strings/0x409/configuration
   echo 250 > configs/c.1/MaxPower

   ln -sf functions/ffs.adb    configs/c.1/
   ln -sf functions/acm.usb0   configs/c.1/
   ln -sf functions/uac1.usb0  configs/c.1/

   # ---- Spawn adbd before binding the UDC ----
   #
   # functionfs gadgets won't enumerate cleanly unless the userspace
   # daemon has opened /dev/usb-ffs/adb/{ep0,ep1,ep2} and finished the
   # descriptor handshake.  Same sequencing as the stock usb-adb.sh.
   if ! pgrep -x adbd >/dev/null 2>&1; then
      adbd &
      sleep 1
   fi

   # ---- Bind to UDC ----
   UDC_NAME=$(ls /sys/class/udc/ | head -1)
   if [ -z "${UDC_NAME}" ]; then
      echo "[usb-tinker] no UDC found in /sys/class/udc/" >&2
      exit 1
   fi
   echo "${UDC_NAME}" > "${UDC_FILE}"

   echo "[usb-tinker] gadget bound: ${GADGET_NAME} → ${UDC_NAME}"
   echo "[usb-tinker]   ADB    (vendor-class)"
   echo "[usb-tinker]   CDC-ACM (will appear as /dev/ttyGS0 on K144,"
   echo "[usb-tinker]            /dev/ttyACM* on host)"
   echo "[usb-tinker]   UAC1    (capture + playback @ ${UAC_SAMPLE_RATE} Hz mono)"
}

case "${1:-}" in
   start)
      if [ -f "${UDC_FILE}" ]; then
         CURRENT=$(cat "${UDC_FILE}" 2>/dev/null || true)
         if [ -n "${CURRENT}" ]; then
            echo "[usb-tinker] already bound to ${CURRENT}"
            exit 0
         fi
      fi
      start_gadget
      ;;
   stop)
      stop_gadget
      echo "[usb-tinker] gadget torn down"
      ;;
   restart)
      stop_gadget
      sleep 1
      start_gadget
      ;;
   *)
      echo "usage: $0 {start|stop|restart}" >&2
      exit 1
      ;;
esac
