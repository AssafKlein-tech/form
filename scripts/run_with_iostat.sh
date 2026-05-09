#!/bin/bash
# Optional sidecar: run mpirun with per-node iostat -x running in the background.
# Produces iostat.<host>.log alongside the FORM logs for the viz script to overlay.
#
# Usage:
#   bash scripts/run_with_iostat.sh -np 60 parform Spin2_h5_45_mr.frm
#
# Honors: FORMTMP for device hint, IOSTAT_INTERVAL for sample period.

set -u

INTERVAL=${IOSTAT_INTERVAL:-1}
FORMTMP_DEV=""
if [ -n "${FORMTMP:-}" ] && [ -e "$FORMTMP" ]; then
    FORMTMP_DEV=$(df --output=source "$FORMTMP" 2>/dev/null | tail -n 1)
    FORMTMP_DEV=$(basename "$FORMTMP_DEV" 2>/dev/null || true)
fi

if ! command -v iostat >/dev/null 2>&1; then
    echo "WARN: iostat not on PATH; running without sidecar." >&2
    exec mpirun "$@"
fi

# Start iostat per node. Inside a PBS job we can use $PBS_NODEFILE; otherwise
# we just run it locally on the head process.
IOSTAT_PIDS=()
start_iostat_local() {
    local host=$(hostname -s)
    local out="iostat.${host}.log"
    if [ -n "$FORMTMP_DEV" ]; then
        nohup iostat -x -t "$INTERVAL" "/dev/$FORMTMP_DEV" >"$out" 2>&1 &
    else
        nohup iostat -x -t "$INTERVAL" >"$out" 2>&1 &
    fi
    IOSTAT_PIDS+=($!)
    echo "iostat[$host] pid=$! -> $out" >&2
}

if [ -n "${PBS_NODEFILE:-}" ] && command -v pbsdsh >/dev/null 2>&1; then
    # Prefer pbsdsh: it places one process per allocated vnode.
    while IFS= read -r host; do
        echo "starting iostat on $host" >&2
        ssh -o StrictHostKeyChecking=no -o BatchMode=yes "$host" \
            "cd $PWD && nohup iostat -x -t $INTERVAL >iostat.\$(hostname -s).log 2>&1 &" || true
    done < <(sort -u "$PBS_NODEFILE")
else
    start_iostat_local
fi

cleanup() {
    if [ ${#IOSTAT_PIDS[@]} -gt 0 ]; then
        kill "${IOSTAT_PIDS[@]}" 2>/dev/null || true
    fi
    if [ -n "${PBS_NODEFILE:-}" ]; then
        while IFS= read -r host; do
            ssh -o StrictHostKeyChecking=no -o BatchMode=yes "$host" \
                "pkill -u \$USER iostat" 2>/dev/null || true
        done < <(sort -u "$PBS_NODEFILE")
    fi
}
trap cleanup EXIT INT TERM

mpirun "$@"
exit_code=$?
exit "$exit_code"
