#!/bin/bash
# nbd-vram-disconnect.sh - deactivate swap and disconnect nbd device
# Called by vram-swap-nbd.service ExecStop.
NBD_DEV=$(cat /run/nbd-vram-dev 2>/dev/null || echo "/dev/nbd0")

swapoff "$NBD_DEV" 2>/dev/null || true
nbd-client -d "$NBD_DEV" 2>/dev/null || true
rm -f /run/nbd-vram-dev

echo "nbd-vram-disconnect: $NBD_DEV released"
