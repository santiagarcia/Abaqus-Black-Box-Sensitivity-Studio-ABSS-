"""Average Abaqus job anatomy over campaign runs (from summary.csv): seconds per job in each phase.

v1-binary runs (pilot, abss_before) are excluded: v1 predates the AM/PM timestamp fix, so their phase split is wrong
(their total times are right).

Usage:  python job_anatomy.py <summary.csv>
"""
import csv
import sys

rows = [r for r in csv.DictReader(open(sys.argv[1])) if r["stage"] != "pilot" and r["method"] != "abss_before" and r["failed"] == "0"
        and r["abaqus_jobs"] not in ("", "0")]


def agg(rs, label):
    jobs = sum(float(r["abaqus_jobs"]) for r in rs)
    s = lambda k: sum(float(r[k] or 0) for r in rs) / jobs
    print(f"{label}: {len(rs)} runs, {jobs:.0f} jobs | driver+licence {s('job_driver_licence_s'):.1f} | "
          f"input processing {s('job_pre_s'):.1f} | Abaqus/Standard {s('job_standard_s'):.1f} | "
          f"wrap-up {s('job_wrapup_s'):.1f} | total {s('abaqus_job_seconds_summed'):.1f} s per job")


agg(rows, "all runs")
agg([r for r in rows if int(r["free_dofs"] or 0) <= 2400], "<= 2,400 free DOFs")
agg([r for r in rows if int(r["free_dofs"] or 0) >= 48672], ">= 48,672 free DOFs")

