#!/usr/bin/env python3
"""MRmpi profile visualization.

Reads pf_profile.csv files produced by parform compiled with --enable-mr-profile
and produces a self-contained interactive HTML report with:

- Per-module phase breakdown (stacked bar, faceted by role).
- Per-rank Gantt for a selected module (dropdown).
- Imbalance heatmap (rank x phase, color = time fraction).
- OS-counter trace (bytes_written, context switches, RSS, disk %util).
- Decision matrix highlighting the dominant bottleneck pattern with
  the recommended next optimization to try.

Usage:
    python pf_profile_viz.py <run_dir>                # single-run report
    python pf_profile_viz.py <mr_dir> <org_dir>       # MR-vs-org compare
"""

import argparse
import os
import sys
from pathlib import Path

import pandas as pd
import plotly.graph_objects as go
from plotly.subplots import make_subplots
import plotly.io as pio


PHASE_COLS = [
    ("t_map_generator_us",     "MAP_GENERATOR",     "mapper"),
    ("t_map_endsort_total_us", "MAP_ENDSORT_TOTAL", "mapper"),
    ("t_map_send_wait_us",     "MAP_SEND_WAIT",     "mapper"),
    ("t_map_send_mpi_us",      "MAP_SEND_MPI",      "mapper"),
    ("t_red_recv_wait_us",     "RED_RECV_WAIT",     "reducer"),
    ("t_red_merge_patches_us", "RED_MERGE_PATCHES", "reducer"),
    ("t_red_final_sort_us",    "RED_FINAL_SORT",    "reducer"),
    ("t_red_forward_wait_us",  "RED_FORWARD_WAIT",  "reducer"),
    ("t_red_forward_mpi_us",   "RED_FORWARD_MPI",   "reducer"),
    ("t_mas_distribute_us",    "MAS_DISTRIBUTE",    "master"),
    ("t_mas_distribute_wait_us","MAS_DISTRIBUTE_WAIT","master"),
    ("t_mas_final_sort_us",    "MAS_FINAL_SORT",    "master"),
    ("t_mas_collect_us",       "MAS_COLLECT",       "master"),
]

ROLE_PHASES = {
    "mapper":  [c for c, _, r in PHASE_COLS if r == "mapper"],
    "reducer": [c for c, _, r in PHASE_COLS if r == "reducer"],
    "master":  [c for c, _, r in PHASE_COLS if r == "master"],
}

DECISION_MATRIX = [
    {
        "id": "reducer_back_pressure",
        "test": lambda d: d["mapper"]["t_map_send_wait_us"] > 0.10 * d["mapper"]["wallclock_us"]
                          and d["reducer"]["t_red_recv_wait_us"] < 0.20 * d["reducer"]["wallclock_us"],
        "diagnosis": "Reducer back-pressured (slow consumer): mappers stall on full sbufs.",
        "knob": "Increase -r<N> (more reducers).",
        "rationale": "More reducers -> less per-reducer load -> mappers don't block on sbuf drain.",
    },
    {
        "id": "reducer_starved",
        "test": lambda d: d["mapper"]["t_map_send_wait_us"] < 0.05 * d["mapper"]["wallclock_us"]
                          and d["reducer"]["t_red_recv_wait_us"] > 0.50 * d["reducer"]["wallclock_us"],
        "diagnosis": "Reducer starved (slow producer): reducers idle waiting for mapper terms.",
        "knob": "Decrease -r<N> (more mappers).",
        "rationale": "More mappers -> faster combined term-generation rate.",
    },
    {
        "id": "disk_bound_io",
        "test": lambda d: d["reducer"]["disk_util_pct"] > 75.0,
        "diagnosis": "Reducers disk-bound (FORMTMP %util > 75%).",
        "knob": "Increase form.set largesize / smallext.",
        "rationale": "Larger in-memory sort -> fewer disk patches -> less I/O.",
    },
    {
        "id": "patch_flush_dominates",
        "test": lambda d: d["reducer"]["t_red_merge_patches_us"] > d["reducer"]["t_red_final_sort_us"]
                          and d["reducer"]["disk_util_pct"] > 50.0,
        "diagnosis": "Patch-flush dominates reducer time.",
        "knob": "Increase form.set filepatches.",
        "rationale": "More patches per merge -> fewer merge passes.",
    },
    {
        "id": "hash_cpu_bound",
        "test": lambda d: d["mapper"]["t_map_hash_pack_us"] > 0.50 * d["mapper"]["wallclock_us"]
                          and d["mapper"]["t_map_send_wait_us"] < 0.10 * d["mapper"]["wallclock_us"],
        "diagnosis": "Mappers hash-pack CPU-bound.",
        "knob": "Code: review the hash kernel (murmur3-AVX512 already in use); consider per-rank load reporting.",
        "rationale": "Hash work is per-term; vectorization is the only lever.",
    },
    {
        "id": "master_merge_bound",
        "test": lambda d: d["master"]["t_mas_final_sort_us"] > 0.50 * d["master"]["wallclock_us"]
                          and d["reducer"]["t_red_recv_wait_us"] > 0.30 * d["reducer"]["wallclock_us"],
        "diagnosis": "Master merge tree dominant; reducers idle.",
        "knob": "Increase number of reducers OR widen master merge tree.",
        "rationale": "Master fan-in scales with numreducers; pre-merging upstream helps.",
    },
    {
        "id": "master_distribute_slow",
        "test": lambda d: d["master"]["t_mas_distribute_wait_us"] > 0.50 * d["master"]["t_mas_distribute_us"]
                          and d["mapper"]["t_map_generator_us"] > 0.0,
        "diagnosis": "Mappers blocked waiting for master to distribute term buckets.",
        "knob": "Bigger mProcessBucketSize OR slower maxinterms ramp-up.",
        "rationale": "Master GetTerm rate is fixed; bigger buckets amortize.",
    },
    {
        "id": "os_oversubscription",
        "test": lambda d: d["mapper"]["nivcsw"] > 50 or d["reducer"]["nivcsw"] > 50,
        "diagnosis": "High involuntary context switches: kernel preemption.",
        "knob": "Reduce ranks per node OR pin with --bind-to core.",
        "rationale": "Preemption is unrelated to FORM workload; OS contention.",
    },
    {
        "id": "reducer_skew",
        "test": lambda d: d["reducer"]["final_sort_var_pct"] > 30.0,
        "diagnosis": "Reducer load skewed (final-sort variance > 30%).",
        "knob": "Code: investigate hash distribution.",
        "rationale": "Some reducers got more terms than others.",
    },
]


def load_csv(run_dir: Path) -> pd.DataFrame:
    csv_path = run_dir / "pf_profile.csv"
    if not csv_path.exists():
        sys.stderr.write(f"ERROR: {csv_path} not found.\n")
        sys.exit(1)
    df = pd.read_csv(csv_path)
    df["t_map_hash_pack_us"] = (
        df["t_map_endsort_total_us"]
        - df["t_map_send_wait_us"]
        - df["t_map_send_mpi_us"]
    ).clip(lower=0)
    df["t_red_store_us"] = (
        df["wallclock_us"]
        - df["t_red_recv_wait_us"]
        - df["t_red_merge_patches_us"]
        - df["t_red_final_sort_us"]
        - df["t_red_forward_wait_us"]
        - df["t_red_forward_mpi_us"]
    ).clip(lower=0)
    leader = df[df["node_disk_time_in_io_ms"] >= 0].copy()
    if not leader.empty:
        leader["disk_util_pct"] = (
            leader["node_disk_time_in_io_ms"] * 1000.0 / leader["node_wallclock_us"].clip(lower=1) * 100.0
        )
        df = df.merge(leader[["module", "rank", "disk_util_pct"]], on=["module", "rank"], how="left")
    else:
        df["disk_util_pct"] = float("nan")
    return df


def role_aggregates(df: pd.DataFrame) -> dict:
    agg = {}
    for role in ("mapper", "reducer", "master"):
        sub = df[df["role"] == role]
        if sub.empty:
            agg[role] = {col: 0.0 for col in df.select_dtypes("number").columns}
            agg[role]["disk_util_pct"] = 0.0
            agg[role]["final_sort_var_pct"] = 0.0
            agg[role]["nivcsw"] = 0
            continue
        d = {col: sub[col].mean() for col in sub.select_dtypes("number").columns}
        d["disk_util_pct"] = sub["disk_util_pct"].dropna().mean() if "disk_util_pct" in sub else 0.0
        if pd.isna(d.get("disk_util_pct", 0.0)):
            d["disk_util_pct"] = 0.0
        if role == "reducer" and "t_red_final_sort_us" in sub:
            mean = sub["t_red_final_sort_us"].mean()
            d["final_sort_var_pct"] = (
                sub["t_red_final_sort_us"].std() / mean * 100.0
                if mean > 0 else 0.0
            )
        d["nivcsw"] = sub["nivcsw"].max() if "nivcsw" in sub else 0
        agg[role] = d
    return agg


def evaluate_decision(df: pd.DataFrame) -> list:
    matched = []
    for module in df["module"].unique():
        m_df = df[df["module"] == module]
        agg = role_aggregates(m_df)
        for rule in DECISION_MATRIX:
            try:
                if rule["test"](agg):
                    matched.append((module, rule))
            except (KeyError, ZeroDivisionError):
                continue
    return matched


def fig_phase_breakdown(df: pd.DataFrame, title_suffix: str = "") -> go.Figure:
    fig = make_subplots(
        rows=1, cols=3, subplot_titles=("Mapper", "Reducer", "Master"),
        shared_yaxes=False,
    )
    for col_idx, role in enumerate(("mapper", "reducer", "master"), start=1):
        sub = df[df["role"] == role]
        for phase_col, phase_label, phase_role in PHASE_COLS:
            if phase_role != role:
                continue
            grouped = sub.groupby("module")[phase_col].mean() / 1.0e6
            fig.add_trace(
                go.Bar(
                    x=grouped.index, y=grouped.values, name=phase_label,
                    legendgroup=phase_label,
                    showlegend=(col_idx == 1),
                ),
                row=1, col=col_idx,
            )
        fig.update_xaxes(title_text="module", row=1, col=col_idx)
        fig.update_yaxes(title_text="time (s)", row=1, col=col_idx)
    fig.update_layout(
        barmode="stack",
        title=f"Phase breakdown per module per role{title_suffix}",
        height=500,
    )
    return fig


def fig_per_rank_gantt(df: pd.DataFrame, module: int) -> go.Figure:
    sub = df[df["module"] == module].sort_values(["role", "rank"])
    fig = go.Figure()
    for phase_col, phase_label, _ in PHASE_COLS:
        fig.add_trace(go.Bar(
            x=sub[phase_col] / 1e6,
            y=[f"r{r} ({role})" for r, role in zip(sub["rank"], sub["role"])],
            orientation="h",
            name=phase_label,
        ))
    fig.update_layout(
        barmode="stack",
        title=f"Per-rank phase breakdown (module {module})",
        xaxis_title="time (s)",
        yaxis_title="rank",
        height=max(300, 25 * len(sub)),
    )
    return fig


def fig_imbalance_heatmap(df: pd.DataFrame) -> go.Figure:
    pivot_cols = [c for c, _, _ in PHASE_COLS]
    rows = []
    for _, row in df.iterrows():
        for phase_col, phase_label, role in PHASE_COLS:
            if row["role"] != role:
                continue
            rows.append({
                "rank": f"r{row['rank']} ({row['role']})",
                "phase": phase_label,
                "module": row["module"],
                "time_s": row[phase_col] / 1e6,
            })
    long = pd.DataFrame(rows)
    if long.empty:
        return go.Figure()
    pivot = long.groupby(["rank", "phase"])["time_s"].sum().unstack(fill_value=0)
    fig = go.Figure(data=go.Heatmap(
        z=pivot.values, x=pivot.columns.tolist(), y=pivot.index.tolist(),
        colorscale="Viridis", colorbar=dict(title="time (s)"),
    ))
    fig.update_layout(
        title="Imbalance heatmap (sum across modules)",
        height=max(300, 22 * len(pivot)),
    )
    return fig


def fig_os_counters(df: pd.DataFrame) -> go.Figure:
    fig = make_subplots(
        rows=2, cols=2,
        subplot_titles=(
            "io_write_bytes per module per role",
            "Involuntary context switches (nivcsw)",
            "MaxRSS (KB)",
            "Disk %util (node-leader rows)",
        ),
    )
    for role in ("mapper", "reducer", "master"):
        sub = df[df["role"] == role]
        if sub.empty:
            continue
        agg = sub.groupby("module").agg(
            io_write_bytes=("io_write_bytes", "sum"),
            nivcsw=("nivcsw", "sum"),
            maxrss_kb=("maxrss_kb", "max"),
        ).reset_index()
        fig.add_trace(go.Scatter(
            x=agg["module"], y=agg["io_write_bytes"], name=f"{role} io_write_bytes",
            mode="lines+markers",
        ), row=1, col=1)
        fig.add_trace(go.Scatter(
            x=agg["module"], y=agg["nivcsw"], name=f"{role} nivcsw",
            mode="lines+markers",
        ), row=1, col=2)
        fig.add_trace(go.Scatter(
            x=agg["module"], y=agg["maxrss_kb"], name=f"{role} maxrss_kb",
            mode="lines+markers",
        ), row=2, col=1)
    leader = df[df["node_disk_time_in_io_ms"] >= 0]
    if not leader.empty and "disk_util_pct" in leader:
        u = leader.groupby("module")["disk_util_pct"].mean()
        fig.add_trace(go.Scatter(
            x=u.index, y=u.values, name="disk %util", mode="lines+markers",
        ), row=2, col=2)
    fig.update_layout(height=700, title="OS counters per module")
    return fig


def fig_decision_matrix(matched: list) -> go.Figure:
    if not matched:
        rows = [["—", "—", "No dominant pattern detected.", "—", "—"]]
    else:
        rows = []
        for module, rule in matched:
            rows.append([
                str(module),
                rule["id"],
                rule["diagnosis"],
                rule["knob"],
                rule["rationale"],
            ])
    fig = go.Figure(data=[go.Table(
        header=dict(
            values=["module", "rule", "diagnosis", "next knob to try", "rationale"],
            fill_color="lightgrey", align="left",
        ),
        cells=dict(
            values=list(zip(*rows)),
            align="left",
        ),
    )])
    fig.update_layout(
        title="Decision matrix: recommended next optimization",
        height=400 + 25 * len(rows),
    )
    return fig


def render_single(run_dir: Path) -> Path:
    df = load_csv(run_dir)
    matched = evaluate_decision(df)
    figs = [
        ("phase_breakdown", fig_phase_breakdown(df)),
        ("decision",        fig_decision_matrix(matched)),
        ("imbalance",       fig_imbalance_heatmap(df)),
        ("os_counters",     fig_os_counters(df)),
    ]
    for module in df["module"].unique():
        figs.append((f"gantt_module_{module}", fig_per_rank_gantt(df, module)))

    out = run_dir / "pf_profile_report.html"
    with open(out, "w") as fh:
        fh.write("<!DOCTYPE html><html><head><meta charset='utf-8'>")
        fh.write(f"<title>pf_profile report — {run_dir}</title>")
        fh.write("<style>body{font-family:sans-serif;max-width:1400px;margin:24px auto;}"
                 "h1,h2{margin-top:32px;} pre{background:#f4f4f4;padding:8px;}</style></head><body>")
        fh.write(f"<h1>MRmpi profile report</h1><p>Source: <code>{run_dir}</code></p>")
        for name, fig in figs:
            fh.write(f"<h2 id='{name}'>{name.replace('_', ' ')}</h2>")
            fh.write(pio.to_html(fig, include_plotlyjs="cdn", full_html=False))
        fh.write("</body></html>")
    return out


def render_compare(mr_dir: Path, org_dir: Path) -> Path:
    mr  = load_csv(mr_dir)
    org = load_csv(org_dir)
    mr["__src"] = "MR"
    org["__src"] = "org"
    combined = pd.concat([mr, org], ignore_index=True)
    fig_phase = make_subplots(
        rows=2, cols=3,
        subplot_titles=("MR mapper", "MR reducer", "MR master",
                        "org mapper", "org reducer", "org master"),
    )
    for row_idx, src in enumerate(("MR", "org"), start=1):
        sub_all = combined[combined["__src"] == src]
        for col_idx, role in enumerate(("mapper", "reducer", "master"), start=1):
            sub = sub_all[sub_all["role"] == role]
            for phase_col, phase_label, phase_role in PHASE_COLS:
                if phase_role != role:
                    continue
                grouped = sub.groupby("module")[phase_col].mean() / 1.0e6
                fig_phase.add_trace(go.Bar(
                    x=grouped.index, y=grouped.values, name=f"{src}/{phase_label}",
                    legendgroup=phase_label,
                    showlegend=(row_idx == 1 and col_idx == 1),
                ), row=row_idx, col=col_idx)
            fig_phase.update_xaxes(title_text="module", row=row_idx, col=col_idx)
            fig_phase.update_yaxes(title_text="time (s)", row=row_idx, col=col_idx)
    fig_phase.update_layout(barmode="stack", height=900,
                            title="Phase breakdown: MR vs org")

    matched_mr  = evaluate_decision(mr)
    matched_org = evaluate_decision(org)

    out = mr_dir / "pf_profile_compare.html"
    with open(out, "w") as fh:
        fh.write("<!DOCTYPE html><html><head><meta charset='utf-8'>")
        fh.write("<title>pf_profile compare — MR vs org</title>")
        fh.write("<style>body{font-family:sans-serif;max-width:1400px;margin:24px auto;}"
                 "h1,h2{margin-top:32px;}</style></head><body>")
        fh.write(f"<h1>MRmpi vs org compare</h1>")
        fh.write(f"<p>MR: <code>{mr_dir}</code><br>org: <code>{org_dir}</code></p>")
        fh.write("<h2>Phase breakdown (MR top, org bottom)</h2>")
        fh.write(pio.to_html(fig_phase, include_plotlyjs="cdn", full_html=False))
        fh.write("<h2>Decision matrix — MR run</h2>")
        fh.write(pio.to_html(fig_decision_matrix(matched_mr),
                             include_plotlyjs=False, full_html=False))
        fh.write("<h2>Decision matrix — org run</h2>")
        fh.write(pio.to_html(fig_decision_matrix(matched_org),
                             include_plotlyjs=False, full_html=False))
        fh.write("</body></html>")
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dirs", nargs="+", type=Path,
                        help="One run dir for single-run report, two for MR-vs-org compare")
    args = parser.parse_args()
    if len(args.dirs) == 1:
        out = render_single(args.dirs[0])
    elif len(args.dirs) == 2:
        out = render_compare(args.dirs[0], args.dirs[1])
    else:
        parser.error("expected 1 or 2 run dirs")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
