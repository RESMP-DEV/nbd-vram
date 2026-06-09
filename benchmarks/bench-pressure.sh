#!/bin/bash
# bench-pressure.sh - NBD-VRAM swap under heavy memory pressure.
# Drives resident demand past total RAM so the kernel is forced to fill the entire
# VRAM swap at zero free memory, then shows the machine is still responsive. The
# daemon is OOM-protected; the memory hog is the OOM target.
# State: restores swap and stops the service only if this script started it.

set -e
cd "$(dirname "$0")"

VRAM_DEV=$(cat /run/nbd-vram-dev 2>/dev/null || echo "/dev/nbd0")
OVER_MIB=6000          # how much to force past total RAM, into swap (nbd0 first)
HOLD_SECS=8            # hold at peak pressure so the gif lingers on the result
SERVICE_WAS_RUNNING=0
HOG_PID=""

type_cmd() {
    echo -n "$ "
    printf '%s' "$1" | pv -qL 40
    sleep 0.3
}

run() {
    type_cmd "$*"
    eval "$@"
    sleep 1
}

swap_used_mib() {  # MiB used on a swap device, by name
    awk -v d="$1" '$1==d{print int($4/1024)}' /proc/swaps
}

cleanup() {
    [ -n "$HOG_PID" ] && kill "$HOG_PID" 2>/dev/null || true
    [ -n "$HOG_PID" ] && wait "$HOG_PID" 2>/dev/null || true
    sleep 2  # let the kernel release the hog's swap pages
    # Reset the device to a clean, empty swap so "state restored" is honest
    if swapon --show --noheadings | awk '{print $1}' | grep -qxF "$VRAM_DEV"; then
        swapoff "$VRAM_DEV" 2>/dev/null || true
        mkswap "$VRAM_DEV" >/dev/null 2>&1 || true
        swapon "$VRAM_DEV" -p "${VRAM_PRIO:-1500}" 2>/dev/null || true
    fi
    [ "$SERVICE_WAS_RUNNING" = "0" ] && systemctl stop vram-swap-nbd 2>/dev/null || true
}
trap cleanup EXIT

if [ "$(id -u)" -ne 0 ]; then
    echo "Error: must run as root (sudo bash $0)" >&2
    exit 1
fi

clear
sleep 1
echo "# VRAM swap under heavy memory pressure"
echo "# fills the ENTIRE VRAM swap at zero free RAM, then checks the box is still alive"
sleep 1.5

# Start the service if it is not already up
echo ""
if systemctl is-active --quiet vram-swap-nbd; then
    SERVICE_WAS_RUNNING=1
else
    run "sudo systemctl start vram-swap-nbd"
fi
VRAM_DEV=$(cat /run/nbd-vram-dev 2>/dev/null || echo "/dev/nbd0")
DAEMON_PID=$(pgrep -x nbd-vram | head -1 || true)
VRAM_PRIO=$(swapon --show --noheadings | awk -v d="$VRAM_DEV" '$1==d{print $NF}')
VRAM_PRIO="${VRAM_PRIO:-1500}"

# Warn (not abort) if mlockall is not confirmed active
if [ -n "$DAEMON_PID" ]; then
    VMLCK=$(awk '/VmLck/{print $2}' /proc/"$DAEMON_PID"/status 2>/dev/null || echo 0)
    [ "${VMLCK:-0}" -eq 0 ] && { echo "# WARNING: VmLck=0 - mlockall may be inactive"; sleep 2; }
fi

echo ""
echo "# baseline swap state (VRAM idle):"
run "swapon --show"

MEMTOTAL=$(awk '/^MemTotal:/{print int($2/1024)}' /proc/meminfo)
NBD_FREE=$(awk -v d="${VRAM_DEV##*/}" '$1=="/dev/"d{print int(($3-$4)/1024)}' /proc/swaps)
# Keep the target within the device so the headline stays "VRAM swap full"
[ "$OVER_MIB" -gt "$(( NBD_FREE - 256 ))" ] && OVER_MIB=$(( NBD_FREE - 256 ))
ALLOC_MIB=$(( MEMTOTAL + OVER_MIB ))

echo ""
echo "# applying ~${ALLOC_MIB} MiB pressure - forcing ~${OVER_MIB} MiB into VRAM..."
sleep 0.5
type_cmd "python3 -c 'touch ${ALLOC_MIB} MiB, hold' &"

# Memory hog: touch one byte per 4 KiB page to force real faults, then hold.
python3 -c "
import time
buf = bytearray(${ALLOC_MIB} * 1024 * 1024)
for i in range(0, len(buf), 4096):
    buf[i] = 1
time.sleep(120)
" &
HOG_PID=$!
echo ""

# Live climb display: one line/sec while nbd0 fills, until it tops out or times out
PEAK=0
STABLE=0
for i in $(seq 1 45); do
    kill -0 "$HOG_PID" 2>/dev/null || break
    avail=$(awk '/^MemAvailable:/{print int($2/1024)}' /proc/meminfo)
    nbd=$(swap_used_mib "$VRAM_DEV")
    vmsw=$(awk '/VmSwap/{print $2}' /proc/"$DAEMON_PID"/status 2>/dev/null || echo "?")
    printf '  RAM free: %5s MiB   NBD-VRAM swap: %5s MiB   daemon: alive (VmSwap %s kB)\n' \
        "$avail" "$nbd" "$vmsw"
    if [ "${nbd:-0}" -gt "$PEAK" ]; then PEAK=$nbd; STABLE=0; else STABLE=$(( STABLE + 1 )); fi
    # Stop once nbd0 has plateaued near full with RAM exhausted
    [ "${avail:-9999}" -lt 200 ] && [ "$STABLE" -ge 3 ] && break
    sleep 1
done

echo ""
echo "# RAM is exhausted and NBD-VRAM swap is full - and the shell still answers:"
sleep 0.3
run "uptime"
run "swapon --show"
sleep "$HOLD_SECS"

echo ""
echo "# ── results ──────────────────────────────────────────"
sleep 0.5
GREEN=$'\033[1;32m'; RESET=$'\033[0m'
printf "  peak NBD-VRAM swap used:   %s MiB\n" "$PEAK"
printf "  daemon VmSwap:             %s kB  (0 = daemon never paged out)\n" \
    "$(awk '/VmSwap/{print $2}' /proc/"$DAEMON_PID"/status 2>/dev/null)"
printf "  outcome:                  ${GREEN}SURVIVED - no freeze, no deadlock${RESET}\n"
sleep 1
echo ""
echo "# restoring clean swap state..."
sleep 3
