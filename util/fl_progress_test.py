#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Acceptance test for PROGRESS=ON (load progress reporting).

Run this script ON the database host, because INPUT is opened by the server:

    CREATE EXTENSION pg_fastload;
    python3 util/fl_progress_test.py -d tpcc -p 5432 -U postgres

It generates its own input file (default 2,000,000 rows) unless --input is
given, creates public.flprog, and checks the NOTICE stream:

  * PROGRESS is off by default, and PROGRESS=OFF stays silent
  * PROGRESS_INTERVAL alone enables reporting
  * a serial load prints the start line, periodic lines and a closing line
  * the closing line reports 100.0% and the exact row count of the SRF
  * percentages never decrease and never exceed 100
  * a parallel load (PARALLEL=ON) reports too, and its closing row count agrees
  * an out-of-range PROGRESS_INTERVAL is rejected
  * enabling progress does not slow a serial load measurably

Exit code: 0 when every check passes, 1 otherwise.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

DDL = "CREATE TABLE public.flprog (c1 text, c2 text, c3 text)"

PROG = re.compile(r"NOTICE:\s+pg_fastload progress: (.*)$")
PCT = re.compile(r"([0-9]+\.[0-9])%")
ROWS = re.compile(r"rows ([\d,]+)")


def run(psql, sql):
    """Run one SQL script; return (progress lines, RESULT line, count, ms, errors)."""
    started = time.time()
    p = subprocess.run(psql + ["-X", "-q", "-A", "-t", "-v", "ON_ERROR_STOP=0"],
                       input=sql, capture_output=True, text=True)
    elapsed = (time.time() - started) * 1000.0

    out = {"lines": [], "result": None, "count": None, "ms": elapsed, "errors": []}
    for line in p.stdout.splitlines():
        line = line.strip()
        if line.startswith("RESULT|"):
            out["result"] = line
            out["count"] = int(line.split("|")[2])
    for line in p.stderr.splitlines():
        line = line.strip()
        m = PROG.search(line)
        if m:
            out["lines"].append(m.group(1).strip())
        elif line.startswith("ERROR:") or line.startswith("psql:"):
            out["errors"].append(line)
    return out


def load_sql(table, path, opts):
    arr = ["TABLE=%s" % table, "INPUT=%s" % path, "TYPE=CSV", "WRITER=DIRECT"] + opts
    arr_sql = ",".join("'%s'" % a.replace("'", "''") for a in arr)
    return ("TRUNCATE %s;\n"
            "SELECT 'RESULT|'||skip||'|'||count||'|'||parse_errors||'|'||"
            "duplicate_new||'|'||duplicate_old "
            "FROM pgfastload.pg_fastload(ARRAY[%s]);\n" % (table, arr_sql))


def make_input(path, rows):
    with open(path, "wb") as f:
        batch = []
        for i in range(rows):
            batch.append(b"%d,value-%d,payload-%d\n" % (i, i, i))
            if len(batch) >= 20000:
                f.write(b"".join(batch))
                batch = []
        f.write(b"".join(batch))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-d", "--dbname", default="postgres")
    ap.add_argument("-p", "--port", default="5432")
    ap.add_argument("-U", "--user", default=os.environ.get("USER", "postgres"))
    ap.add_argument("-H", "--host", default="localhost")
    ap.add_argument("--psql", help="path to psql (default: found on PATH)")
    ap.add_argument("-i", "--input", help="use an existing CSV file instead of generating one")
    ap.add_argument("-n", "--rows", type=int, default=2000000,
                    help="rows to generate when --input is absent (default 2000000)")
    args = ap.parse_args()

    psql = [args.psql or "psql", "-h", args.host, "-p", args.port,
            "-U", args.user, "-d", args.dbname]

    workdir = tempfile.mkdtemp(prefix="flprog.")
    if args.input:
        path = os.path.abspath(args.input)
        total = sum(1 for _ in open(path, "rb"))
    else:
        path = os.path.join(workdir, "flprog.csv")
        total = make_input(path, args.rows)
    print("input: %s (%d rows)" % (path, total))

    setup = subprocess.run(psql + ["-X", "-q", "-A", "-t"],
                           input="DROP TABLE IF EXISTS public.flprog;\n" + DDL + ";\n",
                           capture_output=True, text=True)
    if setup.returncode != 0:
        print(setup.stderr.strip())
        return 1

    # A serial load long enough to need several reports
    interval = 100
    cases = [
        # name, options, expectation
        ("off_default", [], "silent"),
        ("off_explicit", ["PROGRESS=OFF"], "silent"),
        ("serial", ["PARALLEL=OFF", "PROGRESS=ON",
                    "PROGRESS_INTERVAL=%d" % interval], "periodic"),
        ("parallel", ["PARALLEL=ON", "PARALLEL_WORKERS=2", "PROGRESS=ON",
                      "PROGRESS_INTERVAL=%d" % interval], "minimal"),
        ("interval_only", ["PARALLEL=OFF", "PROGRESS_INTERVAL=%d" % interval],
         "periodic"),
        ("bad_interval", ["PROGRESS=ON", "PROGRESS_INTERVAL=50"], "error"),
        ("timing_off", ["PARALLEL=OFF"], "silent"),
        ("timing_on", ["PARALLEL=OFF", "PROGRESS=ON"], "minimal"),
    ]

    bad = 0
    res = {}
    for name, opts, expect in cases:
        r = run(psql, load_sql("public.flprog", path, opts))
        res[name] = r

        if expect == "silent":
            if r["lines"]:
                print("FAIL %-14s expected no progress lines, got %d" % (name, len(r["lines"])))
                bad += 1
            else:
                print("PASS %-14s silent, %d rows" % (name, r["count"] or 0))
            continue

        if expect == "error":
            if any("PROGRESS_INTERVAL" in e for e in r["errors"]):
                print("PASS %-14s rejected: %s" % (name, r["errors"][0]))
            else:
                print("FAIL %-14s expected a PROGRESS_INTERVAL error, got %r"
                      % (name, r["errors"]))
                bad += 1
            continue

        need = 2 if expect == "minimal" else 3
        problems = []
        if len(r["lines"]) < need:
            problems.append("only %d progress lines (want >=%d)" % (len(r["lines"]), need))
        if r["lines"]:
            first, last = r["lines"][0], r["lines"][-1]
            if "start" not in first:
                problems.append("first line is not the start line")
            if "| done" not in last:
                problems.append("last line is not the closing line")
            if "100.0%" not in last:
                problems.append("closing line is not 100.0%")
            m = ROWS.search(last)
            if m and r["count"] is not None and int(m.group(1).replace(",", "")) != r["count"]:
                problems.append("closing rows != SRF count (%d)" % r["count"])
            prev = -1.0
            for ln in r["lines"][1:]:
                mm = PCT.search(ln)
                if not mm:
                    problems.append("line without a percentage: %s" % ln)
                    break
                p = float(mm.group(1))
                if p < prev or p > 100.0:
                    problems.append("percentage out of order or range: %s -> %s" % (prev, p))
                    break
                prev = p

        if problems:
            print("FAIL %-14s %s" % (name, "; ".join(problems)))
            for ln in r["lines"][:3] + r["lines"][-1:]:
                print("     | %s" % ln)
            bad += 1
        else:
            print("PASS %-14s %2d reports, closing rows %s, e.g. %s"
                  % (name, len(r["lines"]), format(r["count"] or 0, ","), r["lines"][-1]))

    if res["timing_off"]["ms"] and res["timing_on"]["ms"]:
        delta = (res["timing_on"]["ms"] - res["timing_off"]["ms"]) / res["timing_off"]["ms"]
        print("     overhead (single run, noisy): %+.1f%%" % (delta * 100))
        if delta > 0.20:
            print("FAIL overhead above 20%%")
            bad += 1

    print("")
    print("RESULT=%s" % ("PASS" if bad == 0 else "FAIL (%d)" % bad))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
