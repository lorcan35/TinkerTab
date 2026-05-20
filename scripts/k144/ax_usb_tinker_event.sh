#!/bin/sh
# K144-side UDC watchdog for the tinker composite gadget (TT #620).
#
# Mirrors the stock /usr/local/m5stack/bin/ax_usb_adb_event.sh — when the
# UDC binding clears (cable unplug, host reboot, etc.) re-bind so the
# gadget reappears automatically.  Without this, every Tab5 reflash or
# cable wiggle requires a physical K144 power-cycle.
#
# Started in background from /etc/rc.local after usb-tinker.sh start.

UDC_FILE=/etc/configfs/usb_gadget/tinker/UDC

while true; do
   if [ -f "${UDC_FILE}" ]; then
      if [ -z "$(cat "${UDC_FILE}")" ]; then
         echo "8000000.dwc3" > "${UDC_FILE}"
      fi
   fi
   sleep 1
done
