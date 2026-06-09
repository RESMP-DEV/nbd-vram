#!/bin/bash
# install.sh - Install nbd-vram VRAM swap (CUDA + NBD, no kernel module needed)
# Run once as root. Survives kernel/driver updates automatically.
#
# Architecture:
#   nbd-vram daemon  - allocates VRAM via CUDA, serves NBD protocol on Unix socket
#   nbd kernel module (built-in) + nbd-client - exposes /dev/nbd0 as a block device
#   systemd service  - manages startup, mkswap, swapon/swapoff lifecycle
#
# Why not the vram_swap kernel module?
#   nvidia_p2p_get_pages_persistent is gated on GeForce/consumer GPUs. The P2P
#   API only works on Quadro/datacenter SKUs. This NBD approach works on any
#   CUDA-capable GPU with no NVIDIA kernel symbols or P2P dependency.

set -e
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"

# Remember a previously-installed VRAM allocation so a reinstall can default to it
PREV_ALLOC=$(grep -oE 'VRAM_SETUP_SIZE_MB=[0-9]+' /etc/systemd/system/vram-swap-nbd.service 2>/dev/null | grep -oE '[0-9]+$' || true)

echo "=== nbd-vram installer ==="
echo "Source: $SRC_DIR"

# Detect an existing install so we can restart it at the end (upgrade path).
# Stop it cleanly via systemd first so ExecStop runs the safe swapoff before we
# replace the binary - never pkill a swap-backing daemon out from under active swap.
SERVICE_WAS_ACTIVE=0
if systemctl is-active --quiet vram-swap-nbd.service 2>/dev/null; then
    SERVICE_WAS_ACTIVE=1
    echo "[pre] vram-swap-nbd.service is running - stopping for upgrade..."
    systemctl stop vram-swap-nbd.service || true
    sleep 1
fi

# Mop up any stray (non-systemd) test instance still holding VRAM
if pgrep -x nbd-vram &>/dev/null; then
    echo "[pre] stopping stray nbd-vram instance..."
    bash "$SRC_DIR/nbd-vram-disconnect.sh" 2>/dev/null || true
    pkill -x nbd-vram 2>/dev/null || true
    sleep 1
fi

# Install config if not already present (no-clobber - preserves user edits on reinstall)
if [ ! -f /etc/nbd-vram.conf ]; then
    install -m 644 "$SRC_DIR/nbd-vram.conf" /etc/nbd-vram.conf

    echo ""
    printf "Enable power-aware management? Auto-disable VRAM swap on battery/low power [y/N]: "
    read -r PM_REPLY || PM_REPLY=""
    if [ "$PM_REPLY" = "y" ] || [ "$PM_REPLY" = "Y" ]; then
        sed -i 's/VRAM_POWER_MANAGEMENT=0/VRAM_POWER_MANAGEMENT=1/' /etc/nbd-vram.conf
        printf "Disable when unplugged from AC? [Y/n]: "
        read -r BAT_REPLY || BAT_REPLY=""
        if [ "$BAT_REPLY" = "n" ] || [ "$BAT_REPLY" = "N" ]; then
            sed -i 's/VRAM_DISABLE_ON_BATTERY=1/VRAM_DISABLE_ON_BATTERY=0/' /etc/nbd-vram.conf
            printf "Disable below battery %% (0 = never) [20]: "
            read -r THRESH || THRESH=""
            THRESH=${THRESH:-20}
            sed -i "s/VRAM_BATTERY_THRESHOLD=20/VRAM_BATTERY_THRESHOLD=${THRESH}/" /etc/nbd-vram.conf
        fi
        echo "Power management enabled. Edit /etc/nbd-vram.conf to change settings later."
    else
        echo "Power management left disabled. Edit /etc/nbd-vram.conf to enable later."
    fi
    echo ""
fi

# Ensure nbd-client is installed
echo "[1/4] Checking dependencies..."
if ! command -v nbd-client &>/dev/null; then
    echo "      installing nbd-client..."
    apt-get install -y nbd-client
fi
echo "      OK"

# Build the daemon
echo "[2/4] Building nbd-vram daemon..."
gcc -O2 -Wall -o "$SRC_DIR/nbd-vram" "$SRC_DIR/nbd-vram.c" -ldl -lpthread
echo "      OK"

# Install binary and service
echo "[3/4] Installing binaries and systemd unit..."
install -m 755 "$SRC_DIR/nbd-vram"                          /usr/local/bin/nbd-vram
install -m 755 "$SRC_DIR/nbd-vram-connect.sh"               /usr/local/bin/nbd-vram-connect.sh
install -m 755 "$SRC_DIR/nbd-vram-disconnect.sh"            /usr/local/bin/nbd-vram-disconnect.sh
install -m 644 "$SRC_DIR/systemd/vram-swap-nbd.service"          /etc/systemd/system/
install -m 755 "$SRC_DIR/nbd-vram-power-check.sh"               /usr/local/bin/nbd-vram-power-check.sh
install -m 644 "$SRC_DIR/systemd/nbd-vram-power-check.service"  /etc/systemd/system/
install -m 644 "$SRC_DIR/systemd/nbd-vram-battery-watch.service" /etc/systemd/system/
install -m 644 "$SRC_DIR/systemd/nbd-vram-battery-watch.timer"  /etc/systemd/system/
mkdir -p /etc/udev/rules.d
install -m 644 "$SRC_DIR/udev/99-nbd-vram-power.rules"          /etc/udev/rules.d/

# Disable old P2P-based services if present
systemctl disable --now vram-setup.service  2>/dev/null || true
systemctl disable --now vram-swapon.service 2>/dev/null || true
systemctl disable --now vram-swap2.service  2>/dev/null || true
echo "      OK"

# Patch thread/connection count to match available CPUs on this machine
NCPU=$(nproc)
sed -i "s/VRAM_NBD_THREADS=.*/VRAM_NBD_THREADS=${NCPU}/" /etc/systemd/system/vram-swap-nbd.service
sed -i "s/VRAM_NBD_CONNECTIONS=.*/VRAM_NBD_CONNECTIONS=${NCPU}/" /etc/systemd/system/vram-swap-nbd.service
echo "      threads/connections set to ${NCPU} (nproc)"

# Ask how much VRAM to dedicate to swap (interactive only, validated). On a
# non-interactive run (e.g. tooling) the service-file default is left untouched.
if [ -t 0 ]; then
    TOTAL_VRAM=$(nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null | head -1 | tr -d ' ')
    # Is this card driving a display? If not (dedicated/secondary GPU, e.g. a
    # hybrid laptop or a workstation with a separate display card) almost all of
    # its VRAM is free to use. If it is, leave headroom for the desktop and games.
    DISP=$(nvidia-smi --query-gpu=display_active --format=csv,noheader 2>/dev/null | head -1 | tr -d ' ')
    REC=""; WHY=""
    if [ -n "$TOTAL_VRAM" ]; then
        if [ "$DISP" = "Disabled" ]; then
            REC=$(( TOTAL_VRAM - 512 ))
            WHY="not driving a display (dedicated or secondary card) - nearly all of its VRAM is free to use"
        else
            REC=$(( TOTAL_VRAM > 3072 ? TOTAL_VRAM - 2048 : TOTAL_VRAM / 2 ))
            WHY="driving your display - headroom is left for the desktop, apps, and games"
        fi
    fi
    DEF=${PREV_ALLOC:-${REC:-7168}}
    echo ""
    if [ -n "$TOTAL_VRAM" ]; then
        echo "Your GPU reports ${TOTAL_VRAM} MiB of VRAM."
        echo "Recommended ${REC} MiB: ${WHY}."
    fi
    while :; do
        printf "VRAM to allocate for swap, in MiB [%s]: " "$DEF"
        read -r ALLOC || ALLOC=""
        ALLOC=${ALLOC:-$DEF}
        case "$ALLOC" in
            ''|*[!0-9]*) echo "  please enter a whole number"; continue ;;
        esac
        if [ "$ALLOC" -lt 1024 ]; then echo "  too small (minimum 1024 MiB)"; continue; fi
        if [ -n "$TOTAL_VRAM" ] && [ "$ALLOC" -gt "$TOTAL_VRAM" ]; then
            echo "  that is more than the GPU has (${TOTAL_VRAM} MiB)"; continue
        fi
        if [ -n "$TOTAL_VRAM" ] && [ "$ALLOC" -gt "$((TOTAL_VRAM - 512))" ]; then
            printf "  that leaves under 512 MiB for the GPU itself - are you sure? [y/N]: "
            read -r SURE || SURE=""
            [ "$SURE" = y ] || [ "$SURE" = Y ] || continue
        fi
        break
    done
    sed -i "s/VRAM_SETUP_SIZE_MB=.*/VRAM_SETUP_SIZE_MB=${ALLOC}/" /etc/systemd/system/vram-swap-nbd.service
    echo "      VRAM allocation set to ${ALLOC} MiB"
fi

# Enable and (re)start
echo "[4/4] Enabling vram-swap-nbd.service..."
systemctl daemon-reload
systemctl enable vram-swap-nbd.service
systemctl enable --now nbd-vram-battery-watch.timer
udevadm control --reload-rules
echo "      OK"

# Bring the freshly installed binary live. restart() starts a stopped unit too,
# so this covers both fresh installs and upgrades. Non-fatal: power management may
# legitimately keep it stopped (e.g. on battery), and that should not fail install.
echo "[5/5] Starting vram-swap-nbd.service..."
if [ "$SERVICE_WAS_ACTIVE" = "1" ]; then
    echo "      (upgrade - restarting to load the new binary)"
fi
if systemctl restart vram-swap-nbd.service; then
    sleep 2
    if systemctl is-active --quiet vram-swap-nbd.service; then
        echo "      OK - swap active:"
        swapon --show | sed 's/^/      /'
    else
        echo "      service did not stay active - check: journalctl -u vram-swap-nbd -n 20"
    fi
else
    echo "      start deferred (power management may have it disabled on battery)"
    echo "      start manually with: sudo systemctl start vram-swap-nbd.service"
fi

echo ""
echo "=== Installation complete ==="
echo ""
echo "To check status:"
echo "  systemctl status vram-swap-nbd"
echo "  swapon --show"
echo "  journalctl -u vram-swap-nbd -n 20"
echo ""
echo "To uninstall:"
echo "  sudo bash uninstall.sh"
