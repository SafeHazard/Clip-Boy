#!/usr/bin/env bash
# run_pcap_finalize.sh -- re-run BOTH pcap on/off matrices with the fixed
# (upward-reprobe) detection, then regenerate the report. sn34k first (badge is
# currently sn34k), then reflash res34rch and run its matrix.
set -uo pipefail
cd "$(dirname "$0")/../.."
PORT="${CLIPBOY_DUT:-COM11}"
OI="scripts/tests/overnight_integration.py"
clean() { powershell -NoProfile -ExecutionPolicy Bypass -File scripts/tests/free_com_port.ps1 >/dev/null 2>&1 || true; }

echo "======== fixed sn34k pcap matrix ========  $(date +%H:%M:%S)"
timeout 1800 py -3 "$OI" --pcap-matrix sn34k || echo "sn34k matrix exited $?"; clean

echo "======== reflash res34rch ========  $(date +%H:%M:%S)"
bash scripts/build.sh --test --res34rch 2>&1 | tail -3 || echo "compile issue"
bash scripts/reliable_flash.sh "$PORT" || echo "flash issue"
sleep 8
py -3 scripts/test_bridge.py --port "$PORT" ping 2>&1 | head -1

echo "======== fixed res34rch pcap matrix ========  $(date +%H:%M:%S)"
timeout 1800 py -3 "$OI" --pcap-matrix res34rch --no-deploy || echo "res34rch matrix exited $?"; clean

echo "======== regenerate report ========  $(date +%H:%M:%S)"
py -3 "$OI" --report || echo "report exited $?"
echo "######## PCAP FINALIZE complete  $(date) ########"
