"""Shared helpers for the flight_cycle_test.sh backlog-trace plotters
(plot_flight_cycle.py + plot_flight_cycle_compare.py): CSV load, the AUC /
catch-up metric, and the shared palette. Kept in one place so the two plotters
can't drift on how the backlog metric is computed."""
import csv

# Palette shared by the flight-cycle plotters.
C_LATEST = "#1f77b4"   # DSU latest (recorded) / backlog-flights bars
C_HWM    = "#2ca02c"   # uploaded S3 manifest hwm / compare run B
C_MB     = "#ff7f0e"   # backlog MB bars
C_RUNA   = "#d62728"   # compare run A


class Trace:
    """One loaded CSV trace + its derived metrics."""
    __slots__ = ("cyc", "typ", "latest", "hwm", "bf", "bmb",
                 "run", "auc_f", "auc_mb", "catch")


def load_trace(path):
    """Load a flight_cycle CSV (columns: cycle,type,dsu_latest,hwm,
    backlog_flights,backlog_mb) and compute the drain metrics.

    Metric: AUC = sum of per-cycle backlog over the run cycles (cycle 0 is the
    seed row, excluded). catch = first run cycle where uploaded hwm caught the
    DSU latest, else None. `type`/`backlog_flights` are optional (the compare
    plotter's CSVs may omit them)."""
    t = Trace()
    t.cyc, t.typ, t.latest, t.hwm, t.bf, t.bmb = [], [], [], [], [], []
    with open(path) as f:
        for r in csv.DictReader(f):
            t.cyc.append(int(r["cycle"]))
            t.typ.append(r.get("type", ""))
            t.latest.append(int(r["dsu_latest"]))
            t.hwm.append(int(r["hwm"]))
            t.bf.append(int(r.get("backlog_flights", 0) or 0))
            t.bmb.append(int(r["backlog_mb"]))
    t.run = [i for i, c in enumerate(t.cyc) if c >= 1]
    t.auc_f = sum(t.bf[i] for i in t.run)
    t.auc_mb = sum(t.bmb[i] for i in t.run)
    t.catch = next((t.cyc[i] for i in t.run if t.hwm[i] >= t.latest[i]), None)
    return t
