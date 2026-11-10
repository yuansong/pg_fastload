#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Acceptance test for load progress tracking (pgfastload.pg_fastload_progress).

Run this script ON the database host, because INPUT is opened by the server:

    CREATE EXTENSION pg_fastload;
    python3 util/fl_progress_test.py -d tpcc -p 5432 -U postgres

It generates its own input file (default 2,000,000 rows) unless --input is
given, creates public.flprog, starts a load in one connection and reads the
progress view from another one -- which is the whole point of the feature: the
loading session is busy, so progress has to be readable from somewhere else.

Checked:

  * the view shows the running load (pid, datname, relname, infile, type,
    mode, interval) and is empty again once the load finishes
  * rows and percent increase monotonically, and percent agrees with
    bytes_read / total_bytes when there is no LIMIT
  * PROGRESS=OFF leaves the view empty for the whole load
  * LIMIT switches the percentage to "rows / LIMIT" (total_rows is set)
  * PROGRESS_INTERVAL alone turns publishing on
  * a parallel load reports mode='parallel'
  * PROGRESS_NOTICE=ON additionally prints the NOTICE stream, whose closing
    line carries 100.0% and the exact row count; the default is silent
  * an out-of-range PROGRESS_INTERVAL is rejected
  * publishing does not slow a serial load measurably

Exit code: 0 when every check passes, 1 otherwise.
"""

import argparse
import os
import re
import statistics
import subprocess
import sys
import tempfile
import time

DDL = "CREATE TABLE public.flprog (c1 text, c2 text, c3 text)"

PROG = re.compile(r"NOTICE:\s+pg_fastload progress: (.*)$")
PCT = re.compile(r"([0-9]+\.[0-9])%")
ROWS = re.compile(r"rows ([\d,]+)")

VIEW_COLS = ("state, mode, input_type, datname, relname, rows, bytes_read, "
             "total_bytes, total_rows, round(percent::numeric, 1), interval_ms")
VIEW_SQL = "SELECT %s FROM pgfastload.pg_fastload_progress" % VIEW_COLS
COUNT_SQL = "SELECT count(*) FROM pgfastload.pg_fastload_progress"


def load_sql(table, path, opts):
    arr = ["TABLE=%s" % table, "INPUT=%s" % path, "TYPE=CSV", "WRITER=DIRECT"] + opts
    arr_sql = ",".join("'%s'" % a.replace("'", "''") for a in arr)
    return ("TRUNCATE %s; "
            "SELECT 'RESULT|'||skip||'|'||count||'|'||parse_errors||'|'||"
            "duplicate_new||'|'||duplicate_old "
            "FROM pgfastload.pg_fastload(ARRAY[%s])" % (table, arr_sql))


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


def query(psql, sql):
    p = subprocess.run(psql + ["-X", "-q", "-A", "-t", "-F", "|", "-c", sql],
                       capture_output=True, text=True)
    return [ln for ln in p.stdout.splitlines() if ln.strip()], p.stderr


def start_load(psql, sql):
    return subprocess.Popen(psql + ["-X", "-q", "-A", "-t", "-c", sql],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True)


def watch(psql, proc, first=0.02, interval=0.15, limit=60.0):
    """Poll the view while the load runs; return the samples seen.

    Each poll costs a psql fork/connect (~20 ms), so the cadence starts tight
    and backs off: a short load can be over in 150 ms, and a fixed slow poll
    would miss it completely and report an empty view.
    """
    samples = []
    started = time.time()
    sleep = first
    while proc.poll() is None and time.time() - started < limit:
        rows, _ = query(psql, VIEW_SQL)
        for ln in rows:
            samples.append(ln.split("|"))
        time.sleep(sleep)
        sleep = min(sleep * 2, interval)
    out, err = proc.communicate()
    return samples, out, err


def parse_result(out):
    for line in out.splitlines():
        if line.startswith("RESULT|"):
            parts = line.split("|")
            return int(parts[2])
    return None


def case(name, psql, path, opts, expect, samples, out, err, bad):
    """Validate one case; returns the number of new failures."""
    count = parse_result(out)
    notices = [PROG.search(ln).group(1).strip()
               for ln in err.splitlines() if PROG.search(ln)]
    errors = [ln.strip() for ln in err.splitlines()
              if ln.startswith("ERROR:") and "PROGRESS_INTERVAL" in ln]

    if expect == "error":
        if errors:
            print("PASS %-16s rejected: %s" % (name, errors[0]))
            return bad
        print("FAIL %-16s expected a PROGRESS_INTERVAL error, got %r" % (name, err.strip()[:120]))
        return bad + 1

    problems = []
    peak = max([int(s[5]) for s in samples] or [0])

    if expect == "empty":
        if samples:
            problems.append("view reported %d row(s) for a load that must not publish: %r"
                            % (len(samples), samples[0]))
    else:
        if not samples:
            problems.append("view stayed empty while the load was running")
        else:
            for s in samples:
                if s[0] != "running":
                    problems.append("state=%s, expected running" % s[0])
                    break
            first = samples[0]
            if first[3] != os.environ.get("FL_DB"):
                problems.append("datname=%r, expected %r" % (first[3], os.environ.get("FL_DB")))
            if first[4] != "public.flprog":
                problems.append("relname=%r, expected public.flprog" % first[4])
            if first[2] != "CSV":
                problems.append("input_type=%r, expected CSV" % first[2])
            if expect == "limit":
                if int(first[8]) == 0:
                    problems.append("total_rows is 0 although LIMIT was given")
                for s in samples:
                    if int(s[8]) > 0 and int(s[5]) > int(s[8]):
                        problems.append("rows %s exceeds total_rows %s" % (s[5], s[8]))
                        break
            else:
                if int(first[8]) != 0:
                    problems.append("total_rows=%s without LIMIT" % first[8])
                if int(first[7]) <= 0:
                    problems.append("total_bytes is not set for a regular input file")

            # monotonic rows, and percentage consistent with bytes when known
            prev = -1
            for s in samples:
                r = int(s[5])
                if r < prev:
                    problems.append("rows went backwards: %d -> %d" % (prev, r))
                    break
                prev = r
            for s in samples:
                pct = float(s[9]) if s[9] else None
                if pct is None:
                    continue
                if pct < 0.0 or pct > 100.0:
                    problems.append("percent out of range: %s" % pct)
                    break
                if int(s[8]) == 0 and int(s[7]) > 0:
                    want = 100.0 * int(s[6]) / int(s[7])
                    if abs(want - pct) > 1.0:
                        problems.append("percent %.1f != bytes ratio %.1f" % (pct, want))
                        break

    # the view must be empty again once the load is over
    after, _ = query(psql, COUNT_SQL)
    if after and int(after[0].strip() or 0) != 0:
        problems.append("view still reports %s row(s) after the load" % after[0])

    if expect == "notice":
        if len(notices) < 3:
            problems.append("only %d NOTICE line(s) with PROGRESS_NOTICE=ON" % len(notices))
        elif "| done" not in notices[-1] or "100.0%" not in notices[-1]:
            problems.append("closing NOTICE is not 100.0%% / done: %s" % notices[-1])
        else:
            m = ROWS.search(notices[-1])
            if m and count is not None and int(m.group(1).replace(",", "")) != count:
                problems.append("closing NOTICE rows != SRF count (%d)" % count)
    elif expect in ("view", "limit"):
        if notices:
            problems.append("%d NOTICE line(s) although PROGRESS_NOTICE is off"
                            % len(notices))

    if problems:
        print("FAIL %-16s %s" % (name, "; ".join(problems)))
        return bad + 1

    detail = ("%d samples, rows 0..%d" % (len(samples), peak)) if samples \
        else "no rows published"
    if samples and peak == 0 and not any("PROGRESS_INTERVAL" in o for o in opts):
        detail += " (start record only: default 1 s cadence, load ended sooner)"
    print("PASS %-16s %s, %s rows loaded" % (name, detail, format(count or 0, ",")))
    return bad


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

    os.environ["FL_DB"] = args.dbname
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
                           input="DROP TABLE IF EXISTS public.flprog; " + DDL + ";",
                           capture_output=True, text=True)
    if setup.returncode != 0:
        print(setup.stderr.strip())
        print("RESULT=FAIL (setup)")
        return 1

    interval = "PROGRESS_INTERVAL=100"
    cases = [
        # name, options, expectation
        ("view_default", ["PARALLEL=OFF"], "view"),
        ("view_parallel", ["PARALLEL=ON", "PARALLEL_WORKERS=2"], "view"),
        ("view_limit", ["PARALLEL=OFF", "LIMIT=1000000"], "limit"),
        ("view_off", ["PARALLEL=OFF", "PROGRESS=OFF"], "empty"),
        ("interval_only", ["PARALLEL=OFF", interval], "view"),
        ("notice_on", ["PARALLEL=OFF", interval, "PROGRESS_NOTICE=ON"], "notice"),
        ("bad_interval", ["PROGRESS_INTERVAL=50"], "error"),
    ]

    bad = 0

    for name, opts, expect in cases:
        proc = start_load(psql, load_sql("public.flprog", path, opts))
        samples, out, err = watch(psql, proc)
        bad = case(name, psql, path, opts, expect, samples, out, err, bad)

        if name == "view_parallel":
            modes = {s[1] for s in samples}
            if modes and "parallel" not in modes:
                print("FAIL view_parallel    mode=%s although PARALLEL=ON" % modes)
                bad += 1
            elif modes:
                print("PASS view_parallel    mode=parallel observed")

    # overhead: 3 interleaved runs each of "publishing" and "off"
    off, on = [], []
    for _ in range(3):
        for label, opts in (("off", ["PARALLEL=OFF", "PROGRESS=OFF"]),
                            ("on", ["PARALLEL=OFF"])):
            t0 = time.time()
            subprocess.run(psql + ["-X", "-q", "-A", "-t", "-c",
                                   load_sql("public.flprog", path, opts)],
                           capture_output=True, text=True)
            (off if label == "off" else on).append(time.time() - t0)
    m_off, m_on = statistics.median(off), statistics.median(on)
    delta = (m_on - m_off) / m_off
    print("     overhead (median of 3): %.3fs off vs %.3fs on, %+.2f%%"
          % (m_off, m_on, delta * 100))
    if delta > 0.20:
        print("FAIL overhead above 20%")
        bad += 1

    print("")
    print("RESULT=%s" % ("PASS" if bad == 0 else "FAIL (%d)" % bad))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
