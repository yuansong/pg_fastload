#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Differential test: pg_fastload vs pg_bulkload (the upstream oracle).

Both extensions must be installed in the same cluster.  Run this script ON the
database host, because INFILE is opened by the server:

    CREATE EXTENSION pg_bulkload;      -- the oracle
    CREATE EXTENSION pg_fastload;      -- the implementation under test

    python3 util/fl_diff_test.py -d tpcc -p 5433 -U postgres
    python3 util/fl_diff_test.py -d tpcc -p 5433 -U postgres -v   # list each case

For every case the same input file is loaded four times -- upstream, ours serial,
ours parallel w=2, ours parallel w=4 -- into a freshly created table, and three
things are compared:

  * the row returned by the loader SRF (skip / count / parse_errors / duplicates),
  * the content digest count(*) || coalesce(sum(hash_record_extended(t,0)),0),
  * the severity of the messages on stderr (ERROR / WARNING / FATAL / PANIC).

"ATOMIC" is the one intentional difference: on a rejected record our parallel path
logs the same WARNING lines but then fails the statement and rolls back (see
docs/USER_MANUAL.md section 11), while the serial path -- like pg_bulkload --
stops reading and keeps the rows written before the error.  An ATOMIC case is
counted as a pass only when the table is left empty.

Exit code: 0 when every comparison passes, 1 otherwise.
"""

import argparse
import io
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

CHUNK = 1024 * 1024
T3 = "CREATE TABLE public.fldiff (c1 text, c2 text, c3 text)"
TYPED = ("CREATE TABLE public.fldiff (id integer, qty numeric(10,2), d date, "
         "ts timestamp, note text)")
UNIQ = ("CREATE TABLE public.fldiff (id integer, v text); "
        "CREATE UNIQUE INDEX fldiff_u ON public.fldiff(id)")
PK = "CREATE TABLE public.fldiff (id integer PRIMARY KEY, v text)"
FIXED = "CREATE TABLE public.fldiff (id integer, name text, amt double precision)"

CASES = []


def case(name, ddl, data, opts=(), pre=None):
    CASES.append({"name": name, "ddl": ddl, "data": data,
                  "opts": list(opts), "pre": pre})


def build_cases():
    rows50 = b"".join(b"%d,%d,%d\n" % (i, i, i) for i in range(50))
    longline = b"A" * (2 * CHUNK + 17) + b"\n"
    dup = b"1,a\n2,b\n1,c\n3,d\n"

    # --- CSV syntax ----------------------------------------------------
    case("plain", T3, b"1,2,3\n4,5,6\n")
    case("quoted", T3, b'"a,b",2,3\n"c""d",5,6\n')
    case("quote_mid", T3, b'a"b,c,d\n')
    case("unterminated_quote", T3, b'"abc,2,3\n')
    case("crlf", T3, b"1,2,3\r\n4,5,6\r\n")
    case("cr_only", T3, b"1,2,3\r4,5,6\r7,8,9")
    case("no_trailing_nl", T3, b"1,2,3\n4,5,6")
    case("blank_line_mid", T3, b"1,2,3\n\n4,5,6\n")
    case("blank_line_tail", T3, b"1,2,3\n4,5,6\n\n")
    case("space_line", T3, b"1,2,3\n   \n4,5,6\n")
    case("extra_cols", T3, b"1,2,3,4\n")
    case("missing_cols", T3, b"1,2\n")
    case("empty_fields", T3, b",,x\ny,,\n")
    case("null_str", T3, b"\\N,a,b\nc,\\N,\\N\n", ["NULL=\\N"])
    case("custom_chars", T3, b"^a|b^|2|3\n4|5|6\n",
         ["DELIMITER=|", "QUOTE=^", "ESCAPE=\\"])
    case("crlf_quoted", T3, b'"a\r\nb",2,3\r\n4,5,6\r\n')

    # --- SKIP / LIMIT ---------------------------------------------------
    case("skip_limit", T3, rows50, ["SKIP=7", "LIMIT=9"])
    case("limit_zero", T3, rows50, ["LIMIT=0"])
    case("skip_all", T3, b"1,2,3\n4,5,6\n", ["SKIP=2"])
    # SKIP counts physical lines, so this window starts after both quote lines
    case("skip_quoted", T3, b'"a\nb,2,3\n4,5,6\n7,8,9\n', ["SKIP=1"])
    case("skip_longline", T3, longline + rows50, ["SKIP=1"])
    case("skip_longline_lim", T3, longline + rows50, ["SKIP=1", "LIMIT=7"])

    # --- chunk-boundary edges (reader reads 1 MiB at a time) ------------
    case("giant_record", T3, b"x" * (1536 * 1024) + b",b,c\n")
    case("giant_quoted", T3, b'"' + b"y" * 1200000 + b"\nzz\n" + b'",b,c\n')
    pad = b"p" * (CHUNK - 1)
    case("quote_at_edge", T3, pad + b'"a\nb,2,3\n' + pad + b'"\n')
    case("utf8_at_edge", T3, b"u" * (CHUNK - 2) + "中文".encode("utf-8") + b"\n1,2,3\n")

    # --- type conversion ------------------------------------------------
    case("typed_ok", TYPED, b"1,12.34,2024-01-02,2024-01-02 03:04:05,ok\n")
    case("typed_bad_int", TYPED,
         b"1,1,2024-01-02,2024-01-02 03:04:05,a\nx,2,2024-01-02,2024-01-02 03:04:05,b\n")
    case("typed_bad_date", TYPED, b"1,1,2024-13-45,2024-01-02 03:04:05,a\n")
    case("typed_empty_num", TYPED, b"1,,2024-01-02,2024-01-02 03:04:05,a\n")

    # --- parse-error policy ---------------------------------------------
    case("parse_errors_2", T3, b"a,b,c\nbad1\nx,y,z\nbad2\nlast,1,2\n",
         ["PARSE_ERRORS=2"])
    case("parse_errors_0", T3, b"a,b,c\nbad1\nx,y,z\n", ["PARSE_ERRORS=0"])

    # --- duplicates ------------------------------------------------------
    case("dup_unique", UNIQ, dup)
    case("dup_keep_old", UNIQ, dup, ["ON_DUPLICATE_KEEP=OLD"])
    case("dup_pk", PK, dup)
    case("dup_preexist", UNIQ, dup, [],
         pre="INSERT INTO public.fldiff VALUES (1,'old1'),(9,'old9');")

    # --- writer / process combinations that must not change results -----
    case("writer_buffered", T3, b"1,2,3\n4,5,6\n", ["WRITER=BUFFERED"])
    case("writer_parallel", T3, b"1,2,3\n4,5,6\n", ["WRITER=PARALLEL"])
    case("multi_process", T3, b"1,2,3\n4,5,6\n",
         ["WRITER=PARALLEL", "MULTI_PROCESS=YES"])
    case("force_not_null", T3, b"\\N,a,b\nc,\\N,d\n",
         ["NULL=\\N", "FORCE_NOT_NULL=c1"])

    # --- fixed length ----------------------------------------------------
    stride = 4 + 10 + 8
    fixed_cols = ["TYPE=FIXED", "STRIDE=%d" % stride, "COL=id INTEGER",
                  "COL=name CHAR(10)", "COL=amt DOUBLE"]
    blob = b"".join(struct.pack("<i", i) + ("n%03d" % i).encode().ljust(10, b" ") +
                    struct.pack("<d", i * 0.5) for i in range(100))
    case("fixed_basic", FIXED, blob, fixed_cols)
    case("fixed_skip", FIXED, blob, fixed_cols + ["SKIP=3"])
    case("fixed_limit", FIXED, blob, fixed_cols + ["LIMIT=4"])
    case("fixed_partial", FIXED, blob + b"\x01\x02\x03\x04\x05", fixed_cols)


def build_big_cases():
    case("big_200k", T3,
         b"".join(b"%d,filler-%07d,%d\n" % (i, i, i % 97) for i in range(200000)))


CONFIGS = [
    ("upstream", "pgbulkload.pg_bulkload", []),
    ("serial", "pgfastload.pg_fastload", ["PARALLEL=OFF"]),
    ("par_w2", "pgfastload.pg_fastload", ["PARALLEL=ON", "PARALLEL_WORKERS=2"]),
    ("par_w4", "pgfastload.pg_fastload", ["PARALLEL=ON", "PARALLEL_WORKERS=4"]),
]

SEV = re.compile(r"\b(ERROR|WARNING|FATAL|PANIC)\b")


def parse_errors_of(result):
    if not result:
        return 0
    fields = result.split("|")
    try:
        return int(fields[3])
    except (IndexError, ValueError):
        return 0


def run_sql(psql_cmd, sql):
    p = subprocess.run(psql_cmd + ["-X", "-q", "-A", "-t", "-v", "ON_ERROR_STOP=0"],
                       input=sql, capture_output=True, text=True)
    out = {"result": None, "digest": None, "sev": [], "msgs": []}
    for line in p.stdout.splitlines():
        line = line.strip()
        if line.startswith("RESULT|"):
            out["result"] = line
        elif line.startswith("DIGEST|"):
            out["digest"] = line
    for line in p.stderr.splitlines():
        m = SEV.search(line)
        if m:
            out["sev"].append(m.group(1))
            out["msgs"].append(line.strip())
    out["sev"].sort()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-d", "--dbname", default="postgres")
    ap.add_argument("-p", "--port", default="5432")
    ap.add_argument("-U", "--user", default=os.environ.get("USER", "postgres"))
    ap.add_argument("-H", "--host", default="localhost")
    ap.add_argument("--big", action="store_true",
                    help="also run the 200k-row case (slower)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    psql = ["psql", "-h", args.host, "-p", args.port, "-U", args.user,
            "-d", args.dbname]

    build_cases()
    if args.big:
        build_big_cases()

    workdir = tempfile.mkdtemp(prefix="fldiff.")
    bad = 0
    atomic = 0
    total = 0

    for spec in CASES:
        path = os.path.join(workdir, spec["name"] + ".dat")
        with open(path, "wb") as f:
            f.write(spec["data"])

        baseline = None
        for tag, srf, extra in CONFIGS:
            opts = ["TABLE=public.fldiff", "INFILE=" + path] + spec["opts"] + extra
            arr = ",".join("'%s'" % o.replace("'", "''") for o in opts)
            sql = "DROP TABLE IF EXISTS public.fldiff;\n%s;\n" % spec["ddl"]
            if spec["pre"]:
                sql += spec["pre"] + "\n"
            sql += ("SELECT 'RESULT|'||skip||'|'||count||'|'||parse_errors||'|'||"
                    "duplicate_new||'|'||duplicate_old FROM %s(ARRAY[%s]);\n"
                    % (srf, arr))
            sql += ("SELECT 'DIGEST|'||count(*)||'|'||"
                    "coalesce(sum(hash_record_extended(t,0)),0) "
                    "FROM public.fldiff t;\n")
            got = run_sql(psql, sql)

            if baseline is None:
                baseline = got
                continue

            total += 1
            problems = []
            if got["digest"] != baseline["digest"]:
                problems.append("digest %s vs %s" % (baseline["digest"], got["digest"]))
            if (got["result"] or "").split("|")[1:] != \
                    (baseline["result"] or "").split("|")[1:]:
                problems.append("result %s vs %s" % (baseline["result"], got["result"]))
            if got["sev"] != baseline["sev"]:
                problems.append("severity %s vs %s" % (baseline["sev"], got["sev"]))

            if problems and tag.startswith("par_") and got["result"] is None \
                    and got["digest"] == "DIGEST|0|0" \
                    and parse_errors_of(baseline["result"]) > 0:
                atomic += 1
                if args.verbose:
                    print("ATOMIC  %-22s %-7s (rejected record, table left empty)"
                          % (spec["name"], tag))
                continue

            if problems:
                bad += 1
                print("FAIL    %-22s %-7s" % (spec["name"], tag))
                for p in problems:
                    print("        " + p)
                if got["msgs"]:
                    print("        %s" % got["msgs"][0][:160])
            elif args.verbose:
                print("PASS    %-22s %-7s %s" % (spec["name"], tag, got["digest"]))

    print("\ncases=%d comparisons=%d failures=%d atomic_aborts=%d"
          % (len(CASES), total, bad, atomic))
    print("RESULT=%s" % ("PASS" if bad == 0 else "FAIL"))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
