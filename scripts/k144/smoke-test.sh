#!/bin/bash
# Round-trip CDC-ACM smoke test — host writes /dev/ttyACM<N>, K144 reads /dev/ttyGS0.
#
# Usage: ./smoke-test.sh /dev/ttyACM1
#        Probes for the right ACM device if no arg given.
#
# Verifies W1 (TT #620) — the Tinker composite gadget's CDC-ACM endpoint
# is wired end-to-end.
set -euo pipefail
ADB="${ADB:-sudo adb}"

# Auto-detect K144's ttyACM (the one with ID_MODEL=ax620e-tinker)
detect_acm() {
   for d in /dev/ttyACM*; do
      [ -c "$d" ] || continue
      if udevadm info "$d" 2>/dev/null | grep -q "ax620e-tinker"; then
         echo "$d"
         return 0
      fi
   done
   echo "" >&2
   echo "could not find a /dev/ttyACM* belonging to K144 (ax620e-tinker)" >&2
   return 1
}

ACM="${1:-$(detect_acm)}"
echo "K144 CDC-ACM device on host: $ACM"

PAYLOAD="TINKER_CDC_PROBE_$(date +%s)_$$"

${ADB} shell "rm -f /tmp/cdc_rx; (cat /dev/ttyGS0 > /tmp/cdc_rx & echo \$! > /tmp/cdc_pid); sleep 4; kill \$(cat /tmp/cdc_pid) 2>/dev/null" &
ADB_PID=$!
sleep 1

sudo bash -c "printf '%s\n' '$PAYLOAD' > $ACM"
sleep 2
wait $ADB_PID 2>/dev/null

RX=$(${ADB} shell "cat /tmp/cdc_rx" 2>/dev/null | tr -d '\r')
${ADB} shell "rm -f /tmp/cdc_rx /tmp/cdc_pid" >/dev/null 2>&1 || true

if [ "$RX" = "$PAYLOAD" ]; then
   echo "PASS: round-trip ok ($PAYLOAD)"
   exit 0
else
   echo "FAIL: expected '$PAYLOAD', got '$RX'"
   exit 1
fi
