#!/usr/bin/env python3
"""Plot a flight_cycle_test.sh backlog trace.

Reads the per-seed CSV (cycle,type,dsu_latest,hwm,backlog_flights,backlog_mb) and
renders the backlog drain: DSU-latest vs uploaded-hwm chase, and the un-uploaded
backlog (flights + MB) per cycle, annotated with the area-under-curve metric and
the catch-up cycle. Usage: plot_flight_cycle.py <csv> [out.png]
"""
import csv, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

csv_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/flight_cycle_seed1.csv"
out_path = sys.argv[2] if len(sys.argv) > 2 else csv_path.rsplit(".", 1)[0] + ".png"

cyc, typ, latest, hwm, bf, bmb = [], [], [], [], [], []
with open(csv_path) as f:
    for r in csv.DictReader(f):
        cyc.append(int(r["cycle"]))
        typ.append(r["type"])
        latest.append(int(r["dsu_latest"]))
        hwm.append(int(r["hwm"]))
        bf.append(int(r["backlog_flights"]))
        bmb.append(int(r["backlog_mb"]))

# Metric: AUC = sum of per-cycle backlog over the run cycles (cycle 0 = seed row).
run = [i for i, c in enumerate(cyc) if c >= 1]
auc_f = sum(bf[i] for i in run)
auc_mb = sum(bmb[i] for i in run)
catch = next((cyc[i] for i in run if hwm[i] >= latest[i]), None)
seed = csv_path.split("seed")[-1].split(".")[0] if "seed" in csv_path else "?"

fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 8), sharex=True)

# Top: the chase — DSU latest (recorded) vs uploaded hwm.
ax1.plot(cyc, latest, "-o", color="#1f77b4", label="DSU latest (recorded)", ms=4)
ax1.plot(cyc, hwm, "-s", color="#2ca02c", label="uploaded (S3 manifest hwm)", ms=4)
ax1.fill_between(cyc, hwm, latest, color="#1f77b4", alpha=0.12, label="backlog (un-uploaded)")
for i in run:  # mark ground cycles
    if typ[i] == "G":
        ax1.axvline(cyc[i], color="gray", ls=":", lw=0.6, alpha=0.5)
if catch:
    ax1.axvline(catch, color="red", ls="--", lw=1.2)
    ax1.annotate(f"caught up\ncycle {catch}", (catch, hwm[run[[cyc[i] for i in run].index(catch)]] if False else max(hwm)),
                 color="red", fontsize=9, ha="center", va="bottom",
                 xytext=(catch, max(latest) * 0.55), textcoords="data")
ax1.set_ylabel("flight number")
ax1.set_title(f"Flight-cycle backlog drain — seed {seed}   "
              f"(AUC={auc_f} flight-cycles / {auc_mb} MB-cycles"
              + (f", caught up cycle {catch})" if catch else ", not caught up)"))
ax1.legend(loc="lower right", fontsize=9)
ax1.grid(alpha=0.3)

# Bottom: backlog magnitude per cycle (flights + MB on twin axes).
ax2.bar([c - 0.18 for c in cyc], bf, width=0.36, color="#1f77b4", alpha=0.7, label="backlog (flights)")
ax2.set_ylabel("backlog (flights)", color="#1f77b4")
ax2.tick_params(axis="y", labelcolor="#1f77b4")
ax2b = ax2.twinx()
ax2b.bar([c + 0.18 for c in cyc], bmb, width=0.36, color="#ff7f0e", alpha=0.7, label="backlog (MB)")
ax2b.set_ylabel("backlog (MB)", color="#ff7f0e")
ax2b.tick_params(axis="y", labelcolor="#ff7f0e")
ax2.set_xlabel("flight cycle  (dotted = quick-ground cycle)")
ax2.grid(alpha=0.3)

fig.tight_layout()
fig.savefig(out_path, dpi=110)
print(f"AUC={auc_f} flight-cycles / {auc_mb} MB-cycles  catchup={catch}  → {out_path}")
