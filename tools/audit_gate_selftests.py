"""DEPRECATED -- superseded by tools/probes/gate_inventory.ps1. Do not resurrect.

Why this file is still here instead of deleted: the runtime refused the delete
(chat-reference protection), and an unconditional delete is not something to
force. So it is neutralised instead: running it now prints the deprecation and
exits 3 (not 0), so nothing can mistake it for a passing gate.

Why it was retired (round 72):
  * gate_inventory.ps1 covers exactly this question -- "can every registered
    probe prove that it fails?" -- and it is registered in run_all.ps1's
    $fastProbes, so it actually runs on every regression.
  * This script was never registered anywhere. Measured in round 72: zero call
    sites across tools/, docs/ and the suite. A meta-gate that nothing executes
    is the exact failure mode gate_inventory exists to prevent.
  * The two also disagreed in framing: this one only counted probes in
    $fastProbes (missing everything registered elsewhere), while gate_inventory
    walks the whole registration and additionally requires a written reason for
    each allow-listed exception.

The measurement that replaces it (same inputs, one source of truth):

    powershell -File tools/probes/gate_inventory.ps1
    [INFO] probes=70 can-fail=66 structural=4 gaps=0
    [PASS] every registered probe can prove it fails, or is allow-listed with a reason
"""
import sys

print('[DEPRECATED] audit_gate_selftests.py is superseded by '
      'tools/probes/gate_inventory.ps1')
print('             that gate is registered in run_all.ps1 and runs on every '
      'regression;')
print('             this script was never called from anywhere (round 72).')
print('             use: powershell -File tools/probes/gate_inventory.ps1')
sys.exit(3)
