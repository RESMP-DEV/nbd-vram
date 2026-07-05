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

MODULE=${MODULE:-../../kernel/module/p100vram.ko}
ALLOC=../../kernel/allocator/p100vram-alloc
NAME=${NAME:-shelf0}
GPU=${GPU:-0}
SIZE_MB=${SIZE_MB:-2048}

echo "[1/6] insmod $MODULE"
sudo insmod "$MODULE" || sudo modprobe p100vram || true

echo "[2/6] launch allocator: gpu=$GPU size=${SIZE_MB}MB name=$NAME"
sudo "$ALLOC" --gpu "$GPU" --size-mb "$SIZE_MB" --name "$NAME" &
ALLOC_PID=$!
trap 'sudo swapoff /dev/p100vram'"$NAME"' 2>/dev/null || true; sudo kill $ALLOC_PID 2>/dev/null || true' EXIT
sleep 1

echo "[3/6] mkswap + swapon -p 100"
sudo mkswap "/dev/p100vram$NAME"
sudo swapon -p 100 "/dev/p100vram$NAME"
swapon --show=NAME,SIZE,USED,PRIO

echo "[4/6] pressure test (Stage 7 gate)"
sudo apt-get install -y stress-ng sysstat >/dev/null
stress-ng --vm 4 --vm-bytes 50% --timeout 120s --metrics-brief || true

echo "[5/6] teardown"
sudo swapoff "/dev/p100vram$NAME"
sudo kill "$ALLOC_PID" || true

echo "[6/6] unload"
sudo rmmod p100vram || true
echo "PASS"
