#!/bin/bash
# bench-iops-parallel.sh - parallel 4K random-read IOPS: NVMe vs NBD-VRAM.
# Uses fio --numjobs=16 so submission fans out across all CPUs and all nbd
# connections - the CONCURRENT memory-pressure workload, where the multi-threaded
# daemon actually scales. (The single-job bench-iops.sh models light/sporadic
# pressure instead.) Both devices run the identical parallel load - a fair fight.
# State: restores VRAM swap when done; stops service only if this script started it.

set -e
cd "$(dirname "$0")"

VRAM_DEV=$(cat /run/nbd-vram-dev 2>/dev/null || echo "/dev/nbd0")
FIO_JOBS=16
FIO_IODEPTH=8
FIO_RUNTIME=15
FIO_SIZE=256m          # per job

find_nvme_dir() {
    for d in /home /opt /var/tmp /tmp; do
        [ -d "$d" ] || continue
        df "$d" 2>/dev/null | grep -q nvme && echo "$d" && return
    done
    echo "/tmp"
}

NVME_DIR=$(find_nvme_dir)
NVME_FILE="$NVME_DIR/.nbd-vram-fiop-tmp"
NVME_PART=$(df "$NVME_DIR" --output=source | tail -1)
SERVICE_WAS_RUNNING=0
FIO_TMP=$(mktemp)

type_cmd() { echo -n "$ "; printf '%s' "$1" | pv -qL 40; sleep 0.3; }
run()      { type_cmd "$*"; eval "$@"; sleep 1; }
fio_bench(){ eval "$@" 2>&1 | tee "$FIO_TMP"; }
parse_iops(){ grep -oP "${1}:.*?IOPS=\K[\d.]+[k]?" "$FIO_TMP" | head -1; }
parse_bw()  { grep -oP "${1}:.*?BW=\K[^ ,)]+"       "$FIO_TMP" | head -1; }
iops_num()  { awk -v s="$1" 'BEGIN{n=s+0; if(s ~ /k/) n*=1000; print n}'; }

cleanup() {
    rm -f "$NVME_FILE"* "$FIO_TMP"
    if ! swapon --show --noheadings | awk '{print $1}' | grep -qxF "$VRAM_DEV"; then
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

command -v fio >/dev/null || { echo "installing fio..."; apt-get install -y fio >/dev/null; }

clear
sleep 1
echo "# Parallel 4K random-read IOPS - NVMe vs NBD-VRAM"
echo "# fio --numjobs=${FIO_JOBS} --iodepth=${FIO_IODEPTH} (concurrent pressure: every CPU submitting at once)"
sleep 1.5

# ── 1/2  NVMe ───────────────────────────────────────────────────
echo ""
echo "# ── 1/2  NVMe  ($NVME_PART) ──"
sleep 0.5
echo ""
type_cmd "fio --name=nvme --filename=$NVME_FILE --ioengine=libaio --direct=1 --rw=randread --bs=4k --size=$FIO_SIZE --numjobs=$FIO_JOBS --iodepth=$FIO_IODEPTH --runtime=$FIO_RUNTIME --time_based --group_reporting"
fio_bench "fio --name=nvme --filename=$NVME_FILE --ioengine=libaio --direct=1 --rw=randread --bs=4k --size=$FIO_SIZE --numjobs=$FIO_JOBS --iodepth=$FIO_IODEPTH --runtime=$FIO_RUNTIME --time_based --group_reporting"
NVME_IOPS=$(parse_iops read); NVME_BW=$(parse_bw read)
rm -f "$NVME_FILE"*
sleep 1.5

# ── 2/2  NBD-VRAM ───────────────────────────────────────────────
echo ""
echo "# ── 2/2  NBD-VRAM — start service ──"
sleep 0.5
if systemctl is-active --quiet vram-swap-nbd; then SERVICE_WAS_RUNNING=1; else run "sudo systemctl start vram-swap-nbd"; fi
VRAM_DEV=$(cat /run/nbd-vram-dev 2>/dev/null || echo "/dev/nbd0")
VRAM_PRIO=$(swapon --show --noheadings | awk -v d="$VRAM_DEV" '$1==d{print $NF}'); VRAM_PRIO="${VRAM_PRIO:-1500}"
CONNS=$(ss -x 2>/dev/null | grep -c nbd-vram.sock)

echo ""
type_cmd "swapoff $VRAM_DEV"
swapoff "$VRAM_DEV"
sleep 0.5
echo ""
type_cmd "fio --name=vram --filename=$VRAM_DEV --ioengine=libaio --direct=1 --rw=randread --bs=4k --numjobs=$FIO_JOBS --iodepth=$FIO_IODEPTH --runtime=$FIO_RUNTIME --time_based --group_reporting"
fio_bench "fio --name=vram --filename=$VRAM_DEV --ioengine=libaio --direct=1 --rw=randread --bs=4k --numjobs=$FIO_JOBS --iodepth=$FIO_IODEPTH --runtime=$FIO_RUNTIME --time_based --group_reporting"
VRAM_IOPS=$(parse_iops read); VRAM_BW=$(parse_bw read)

echo ""
type_cmd "mkswap $VRAM_DEV && swapon $VRAM_DEV -p $VRAM_PRIO"
mkswap "$VRAM_DEV" >/dev/null
swapon "$VRAM_DEV" -p "$VRAM_PRIO"
sleep 1.5

# ── results ─────────────────────────────────────────────────────
echo ""
echo "# ── results (parallel 4K randread, ${FIO_JOBS} jobs x QD${FIO_IODEPTH}) ──"
sleep 0.5
# highlight the higher-IOPS device
GREEN=$'\033[1;32m'; RESET=$'\033[0m'; NV=""; VR=""
if awk "BEGIN{exit !( $(iops_num "$NVME_IOPS") >= $(iops_num "$VRAM_IOPS") )}"; then NV=$GREEN; else VR=$GREEN; fi
printf "  ${NV}%-22s  %-12s  %s${RESET}\n" "NVMe ($NVME_PART)"   "${NVME_IOPS} IOPS" "$NVME_BW"
printf "  ${VR}%-22s  %-12s  %s${RESET}\n" "NBD-VRAM ($VRAM_DEV, ${CONNS} conns)" "${VRAM_IOPS} IOPS" "$VRAM_BW"
[ -n "${BENCH_RESULT_FILE:-}" ] && printf '%s\t%s\n' "$(iops_num "$NVME_IOPS")" "$(iops_num "$VRAM_IOPS")" >> "$BENCH_RESULT_FILE"
sleep 2
