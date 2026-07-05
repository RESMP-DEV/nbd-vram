#!/usr/bin/env bash
# test/integration/run-on-host.sh
#
# Host-only integration: load the kmod, mkswap a /dev/p100vram* device,
# swapon at priority 100, run the nbd-vram benchmark harnesses against
# it, then swapoff + unload.
#
# MUST be skipped on macOS - it cannot run here. The make target prints
# a clear message instead.
set -euo pipefail

if [ "$(uname -s)" != "Linux" ]; then
    echo "[skip] integration tests require Linux + NVIDIA P100" >&2
    exit 1
fi

# Resolve paths from the script's own location so the harness works
# regardless of the caller's CWD (e.g. invoked as
# 'test/integration/run-on-host.sh' from the repo root).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

MODULE=${MODULE:-"$REPO_ROOT/kernel/module/p100vram.ko"}
ALLOC=${ALLOC:-"$REPO_ROOT/kernel/allocator/p100vram-alloc"}
NAME=${NAME:-shelf0}
GPU=${GPU:-0}
SIZE_MB=${SIZE_MB:-2048}
DEV="/dev/p100vram${NAME}"

echo "[1/6] insmod $MODULE"
sudo insmod "$MODULE" || sudo modprobe p100vram || true

echo "[2/6] launch allocator: gpu=$GPU size=${SIZE_MB}MB name=$NAME"
sudo "$ALLOC" --gpu "$GPU" --size-mb "$SIZE_MB" --name "$NAME" &
ALLOC_PID=$!
cleanup() {
    sudo swapoff "$DEV" 2>/dev/null || true
    sudo kill "$ALLOC_PID" 2>/dev/null || true
}
trap cleanup EXIT
sleep 1

echo "[3/6] mkswap + swapon -p 100"
sudo mkswap "$DEV"
sudo swapon -p 100 "$DEV"
swapon --show=NAME,SIZE,USED,PRIO

echo "[4/6] pressure test (Stage 7 gate)"
sudo apt-get install -y stress-ng sysstat >/dev/null
# Do NOT mask stress-ng failures: this is the Stage 7 survival gate, so a
# crash under memory pressure is exactly the failure mode we want to
# surface. Letting the command fail propagates a non-zero exit instead of
# printing a false PASS.
stress-ng --vm 4 --vm-bytes 50% --timeout 120s --metrics-brief

echo "[5/6] teardown"
sudo swapoff "$DEV"
sudo kill "$ALLOC_PID" || true
trap - EXIT

echo "[6/6] unload"
sudo rmmod p100vram || true
echo "PASS"
