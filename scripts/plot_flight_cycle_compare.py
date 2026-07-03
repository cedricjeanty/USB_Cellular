#!/usr/bin/env python3
"""Overlay two flight_cycle_test.sh backlog traces (e.g. compression A/B).

Usage: plot_flight_cycle_compare.py <csvA> <labelA> <csvB> <labelB> [out.png]
Plots both backlog-MB drain curves + uploaded-hwm, annotated with each run's
area-under-curve (MB-cycles) and catch-up cycle, so the lever's effect is visible.
"""
import sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from flight_cycle_common import load_trace, C_HWM, C_RUNA

csvA, labelA, csvB, labelB = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
out = sys.argv[5] if len(sys.argv) > 5 else "/tmp/flight_backlog_compare.png"

A = load_trace(csvA)
B = load_trace(csvB)

fig, ax = plt.subplots(figsize=(10, 6))
cA, bmbA, aucA, catchA = A.cyc, A.bmb, A.auc_mb, A.catch
cB, bmbB, aucB, catchB = B.cyc, B.bmb, B.auc_mb, B.catch
ax.plot(cA, bmbA, "-o", color=C_RUNA, ms=5,
        label=f"{labelA}: AUC {aucA} MB-cyc" + (f", caught up cyc {catchA}" if catchA else ", not caught up"))
ax.plot(cB, bmbB, "-s", color=C_HWM, ms=5,
        label=f"{labelB}: AUC {aucB} MB-cyc" + (f", caught up cyc {catchB}" if catchB else ", not caught up"))
if catchA: ax.axvline(catchA, color=C_RUNA, ls="--", lw=1, alpha=0.6)
if catchB: ax.axvline(catchB, color=C_HWM, ls="--", lw=1, alpha=0.6)

speedup = f"{aucA/aucB:.1f}x lower AUC" if aucB else ""
ax.set_title(f"Backlog drain: {labelA} vs {labelB}   ({speedup})")
ax.set_xlabel("flight cycle")
ax.set_ylabel("un-uploaded backlog (MB)")
ax.legend(loc="upper right")
ax.grid(alpha=0.3)
ax.set_ylim(bottom=0)

fig.tight_layout()
fig.savefig(out, dpi=110)
print(f"{labelA}: AUC={aucA}MBc catchup={catchA}  |  {labelB}: AUC={aucB}MBc catchup={catchB}  → {out}")
