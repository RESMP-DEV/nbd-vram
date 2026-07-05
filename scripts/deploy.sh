#!/usr/bin/env bash
# scripts/deploy.sh - rsync the tree to the P100 host and build there.
#
# Configure the host via env:
#   P100_HOST=tesla-box.local  P100_USER=ubuntu  scripts/deploy.sh
set -euo pipefail

HOST="${P100_HOST:?set P100_HOST to the P100 dev box hostname/IP}"
USER="${P100_USER:-$USER}"
DEST="${P100_DEST:-/home/$USER/p100vram-shelf}"

HERE="$(cd "$(dirname "$0")/.." && pwd)"
echo "[deploy] rsync $HERE -> $USER@$HOST:$DEST"
rsync -az --delete \
    --exclude '.git' --exclude '*.o' --exclude '*.ko' --exclude '*.a' \
    --exclude 'nbd-vram' --exclude 'p100vram-alloc' --exclude 'p100vram-sidecar' \
    --exclude 'unit_tests' \
    "$HERE/" "$USER@$HOST:$DEST/"

echo "[deploy] remote build + unit tests"
ssh "$USER@$HOST" "cd '$DEST' && make clean && make alloc gdr sidecar && make test"

echo "[deploy] (kmod build is Linux-only; run on host: make kmod)"
ssh "$USER@$HOST" "cd '$DEST' && make kmod 2>&1 | tail -20 || true"

echo "[deploy] done"
