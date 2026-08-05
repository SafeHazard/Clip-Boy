#!/usr/bin/env bash
# run_overnight_all.sh -- full Clip-Boy tool independence/integration run.
#
# Res34rch: 2 random shuffle passes + PCAP on/off matrix.
# Reflash sn34k (ROM loader), then Sn34k: 2 shuffle passes + PCAP matrix.
# Then aggregate the morning report.
#
# NOT set -e: a single tool/pass erroring must not abort the night. Each phase is
# `timeout`-bounded so a badge wedge can't hang the whole run -- the orchestrator
# also self-recovers reboots/sessions internally.
set -uo pipefail
cd "$(dirname "$0")/../.."   # -> ui_test root
PORT="${CLIPBOY_DUT:-COM11}"
OI="scripts/tests/overnight_integration.py"
STAMP=$(date +%Y%m%d_%H%M%S)
echo "############ OVERNIGHT INTEGRATION RUN  start $(date) ############"
echo "DUT=$PORT  witness=${CLIPBOY_WITNESS:-COM8}  kalipi=${CLIPBOY_KALIPI:-192.168.1.146}"

phase() { echo; echo "======== $* ========  $(date +%H:%M:%S)"; }
# Kill any orphaned pass/bridge python between phases so a timeout-killed phase
# can't hold COM11 and cascade-fail the next (Windows TaskStop/timeout does not
# tree-kill the grandchild pass process).
cleanup() { powershell -NoProfile -ExecutionPolicy Bypass -File scripts/tests/free_com_port.ps1 >/dev/null 2>&1 || true; }

# ---------------- RES34RCH (already flashed) ----------------
phase "R1 res34rch shuffle seed=11"
timeout 7200 py -3 "$OI" --pass res34rch --pass-no 1 --seed 11 || echo "R1 exited $?"; cleanup
phase "R2 res34rch shuffle seed=22"
timeout 7200 py -3 "$OI" --pass res34rch --pass-no 2 --seed 22 --no-deploy || echo "R2 exited $?"; cleanup
phase "R-PCAP res34rch pcap matrix"
timeout 3000 py -3 "$OI" --pcap-matrix res34rch --no-deploy || echo "R-PCAP exited $?"; cleanup

# ---------------- reflash SN34K ----------------
phase "reflash sn34k (--test)"
bash scripts/build.sh --test 2>&1 | tail -5 || echo "sn34k compile issue"
bash scripts/reliable_flash.sh "$PORT" || echo "sn34k flash issue"
sleep 8
py -3 scripts/test_bridge.py --port "$PORT" ping 2>&1 | head -1

# ---------------- SN34K ----------------
phase "S1 sn34k shuffle seed=33"
timeout 7200 py -3 "$OI" --pass sn34k --pass-no 1 --seed 33 --no-deploy || echo "S1 exited $?"; cleanup
phase "S2 sn34k shuffle seed=44"
timeout 7200 py -3 "$OI" --pass sn34k --pass-no 2 --seed 44 --no-deploy || echo "S2 exited $?"; cleanup
phase "S-PCAP sn34k pcap matrix"
timeout 3000 py -3 "$OI" --pcap-matrix sn34k --no-deploy || echo "S-PCAP exited $?"; cleanup

# ---------------- REPORT ----------------
phase "aggregate report"
py -3 "$OI" --report || echo "report exited $?"

echo "############ OVERNIGHT RUN complete  $(date) ############"
