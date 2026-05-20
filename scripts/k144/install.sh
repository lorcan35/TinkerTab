#!/bin/bash
# Install the Tinker composite USB gadget on a K144 (M5Stack LLM Module).
#
# Run from the dev host that has ADB access to the K144's top USB-C
# (vendor 32c9 product 2003).  Idempotent — re-running is safe.
#
# TT #620
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ADB="${ADB:-sudo adb}"

step() { printf "\n\033[1;36m== %s ==\033[0m\n" "$*"; }

step "Confirming ADB sees the K144"
${ADB} kill-server >/dev/null 2>&1 || true
${ADB} start-server >/dev/null 2>&1
sleep 1
if ! ${ADB} devices | grep -q "axera-ax620e"; then
   echo "K144 not visible via ADB.  Plug K144 USB-C into this host first." >&2
   exit 1
fi

step "Pushing /soc/scripts/usb-tinker.sh"
${ADB} push "${SCRIPT_DIR}/usb-tinker.sh" /soc/scripts/usb-tinker.sh
${ADB} shell "chmod 755 /soc/scripts/usb-tinker.sh"

step "Backing up original usb-adb.sh + rc.local (idempotent)"
${ADB} shell "[ -f /soc/scripts/usb-adb.sh.orig ] || cp /soc/scripts/usb-adb.sh /soc/scripts/usb-adb.sh.orig"
${ADB} shell "[ -f /etc/rc.local.orig ] || cp /etc/rc.local /etc/rc.local.orig"

step "Pointing rc.local at usb-tinker.sh (replaces usb-adb.sh)"
${ADB} shell "sed -i 's|/soc/scripts/usb-adb.sh start|/soc/scripts/usb-tinker.sh start|g' /etc/rc.local"

step "Disabling legacy ax_usb_adb_event.sh watchdog (targets old gadget name)"
${ADB} shell "sed -i 's|^/usr/local/m5stack/bin/ax_usb_adb_event.sh|#disabled-for-tinker # /usr/local/m5stack/bin/ax_usb_adb_event.sh|' /etc/rc.local"

step "Verifying rc.local edit"
${ADB} shell "grep -n 'usb-tinker\\|usb-adb\\|ax_usb_adb_event' /etc/rc.local /etc/rc.local.orig"

step "sync + reboot to apply"
${ADB} shell "sync"
${ADB} reboot

step "Waiting for K144 composite to enumerate"
for i in $(seq 1 30); do
   if lsusb | grep -q "32c9:2003"; then
      echo "K144 visible at ~$((i*4))s"
      break
   fi
   sleep 4
done

sleep 2
step "Composite verification"
lsusb -d 32c9:2003 -v 2>/dev/null | grep -E "iProduct|bNumInterfaces" | head
echo "---"
ls /dev/ttyACM* 2>&1 | head
echo "---"
cat /proc/asound/cards | grep -A1 -i tinker || true
echo "---"
${ADB} kill-server >/dev/null 2>&1
${ADB} start-server >/dev/null 2>&1
sleep 2
${ADB} devices

echo ""
echo "Done.  K144 now exposes: ADB + CDC-ACM (/dev/ttyACM*) + UAC1 (USB-Audio card)."
echo "Roll back: ${ADB} shell 'cp /etc/rc.local.orig /etc/rc.local && reboot'"
