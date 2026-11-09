#!/bin/bash
# Functional acceptance test for pg_fastload against one PostgreSQL major.
#
#   usage: util/fl_ver_test.sh <major>
#
# Environment overrides:
#   PG_PREFIX   PostgreSQL install prefix       (default: $HOME/pg<major>)
#   SRC         pg_fastload source tree         (default: the repository root)
#   ROWS        generated rows per input file    (default: 200000)
#   DATA_ROOT   where the throw-away cluster lives (default: /tmp)
#
# What it does: builds the extension against <PG_PREFIX>, starts a throw-away
# cluster on port 54<major>, and checks serial vs parallel CSV and fixed-length
# loads, the FILTER downgrade, parse-error handling and the CLI client against
# an order-independent content digest.
set -u
M="${1:?major version required}"
PG="${PG_PREFIX:-$HOME/pg$M}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${SRC:-$(cd "$HERE/.." && pwd)}"
DATA_ROOT="${DATA_ROOT:-/tmp}"
PORT="${PORT:-54$M}"
DATA="$DATA_ROOT/pgdata$M"
SOCK="$DATA_ROOT"
OUT="${OUT:-$DATA_ROOT/flver_$M.log}"
ROWS="${ROWS:-200000}"

: > "$OUT"
say() { echo "$*" | tee -a "$OUT"; }
fail() { say "FAIL: $*"; echo "RESULT_${M}=FAIL" | tee -a "$OUT"; exit 1; }

say "=== pg_fastload on PostgreSQL $M ==="
say "source: $SRC"
say "prefix: $PG"
"$PG/bin/postgres" --version | tee -a "$OUT" || fail "no postgres binary"

say "--- stop any old cluster on $PORT ---"
"$PG/bin/pg_ctl" -D "$DATA" -m immediate stop >>"$OUT" 2>&1
rm -rf "$DATA"

say "--- build extension ---"
cd "$SRC" || fail "no source tree"
# NB: clean must be driven by the target version's pg_config, otherwise PGXS is
# not loaded at all (when no bare pg_config is on PATH) and stale .o files built
# against another major silently get reused.
make USE_PGXS=1 PG_CONFIG="$PG/bin/pg_config" clean >>"$OUT" 2>&1
if ! make USE_PGXS=1 PG_CONFIG="$PG/bin/pg_config" >>"$OUT" 2>&1; then
  say "BUILD_ERRORS:"
  grep -E "error:" "$OUT" | head -30 | tee -a "$OUT"
  fail "build"
fi
make USE_PGXS=1 PG_CONFIG="$PG/bin/pg_config" install >>"$OUT" 2>&1 || fail "install"
say "build+install ok"

say "--- initdb ---"
"$PG/bin/initdb" -D "$DATA" -U postgres -A trust --no-sync >>"$OUT" 2>&1 || fail "initdb"
cat >> "$DATA/postgresql.conf" <<EOF
port = $PORT
listen_addresses = 'localhost'
unix_socket_directories = '$SOCK'
max_worker_processes = 32
shared_buffers = 256MB
fsync = off
synchronous_commit = off
full_page_writes = off
log_min_messages = warning
EOF
"$PG/bin/pg_ctl" -D "$DATA" -l "$DATA/server.log" -w start >>"$OUT" 2>&1 || fail "start"

PSQL="$PG/bin/psql -h $SOCK -p $PORT -U postgres -At"
$PSQL -d postgres -c "CREATE DATABASE tpcc;" >>"$OUT" 2>&1
P="$PG/bin/psql -h $SOCK -p $PORT -U postgres -d tpcc -At"
$P -c "CREATE EXTENSION pg_fastload;" >>"$OUT" 2>&1 || fail "create extension"
say "extension created: $($P -c "SELECT extversion FROM pg_extension WHERE extname='pg_fastload';")"

say "--- generate test data ($ROWS rows) ---"
awk -v n="$ROWS" 'BEGIN{for(i=0;i<n;i++){printf "row%d,%d,%d,%.4f,\"q,%d\"\n", i, i%97, i*3, i/7.0, i}}' > "$DATA/fl_mv.csv"
say "csv bytes: $(stat -c %s "$DATA/fl_mv.csv")"
CSVFILE="$DATA/fl_mv.csv"
BINFILE="$DATA/fl_mv.bin"

# 100-byte fixed-length records: CHAR(16) INT4 BIGINT FLOAT8 CHAR(64)
cat > "$DATA/fl_gen_fixed.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv){
    long n = atol(argv[1]); FILE *f = fopen(argv[2], "wb");
    char r[100]; long i; int k;
    if(!f) return 1;
    for(i=0;i<n;i++){
        memset(r,' ',100);
        snprintf(r,17,"row%013ld",i);
        { int v=(int)(i%1000000); memcpy(r+16,&v,4); }
        { long v=i*7919; memcpy(r+20,&v,8); }
        { double v=i/7.0; memcpy(r+28,&v,8); }
        for(k=36;k<100;k++) r[k]=(char)('a'+((i+k)%26));
        fwrite(r,1,100,f);
    }
    fclose(f); return 0;
}
EOF
gcc -O2 -o "$DATA/fl_gen_fixed" "$DATA/fl_gen_fixed.c" || fail "gcc fixed gen"
"$DATA/fl_gen_fixed" "$ROWS" "$BINFILE"
say "fixed bytes: $(stat -c %s "$BINFILE")"

DIGEST="SELECT count(*)||'|'||coalesce(sum(hash_record_extended(t,0))::text,'0') FROM %s t;"

run_csv() {   # $1=tag  $2...=extra opts
  local tag="$1"; shift
  local opts="'INPUT=$CSVFILE','OUTPUT=public.mv','TYPE=CSV','WRITER=DIRECT'"
  for o in "$@"; do opts="$opts,'$o'"; done
  $P -c "TRUNCATE public.mv;" >/dev/null 2>&1
  $P -c "SET client_min_messages=INFO; SELECT count FROM pgfastload.pg_fastload(ARRAY[$opts]);" 2>&1 | tail -1
}

run_fixed() {
  local tag="$1"; shift
  local opts="'INPUT=$BINFILE','OUTPUT=public.mvf','TYPE=BINARY','WRITER=DIRECT','COL=CHAR(16)','COL=INTEGER(4)','COL=BIGINT(8)','COL=DOUBLE(8)','COL=CHAR(64)'"
  for o in "$@"; do opts="$opts,'$o'"; done
  $P -c "TRUNCATE public.mvf;" >/dev/null 2>&1
  $P -c "SET client_min_messages=INFO; SELECT count FROM pgfastload.pg_fastload(ARRAY[$opts]);" 2>&1 | tail -1
}

say "--- CSV: serial vs parallel ---"
$P -c "DROP TABLE IF EXISTS public.mv; CREATE TABLE public.mv(c1 text, c2 int, c3 bigint, c4 numeric, c5 text);" >>"$OUT" 2>&1
cd_off=$(run_csv off "PARALLEL=OFF")
d_off=$($P -c "$(printf "$DIGEST" public.mv)" | tail -1)
say "  off   loaded=$cd_off digest=$d_off"

for w in 2 4 8; do
  c=$(run_csv w$w "PARALLEL=ON" "PARALLEL_WORKERS=$w")
  d=$($P -c "$(printf "$DIGEST" public.mv)" | tail -1)
  say "  w$w    loaded=$c digest=$d $([ "$d" = "$d_off" ] && echo MATCH || echo MISMATCH)"
  [ "$d" = "$d_off" ] || fail "csv w$w digest"
  [ "$c" = "$ROWS" ] || fail "csv w$w count=$c"
done

c=$(run_csv auto "PARALLEL=ON")
d=$($P -c "$(printf "$DIGEST" public.mv)" | tail -1)
say "  auto  loaded=$c digest=$d $([ "$d" = "$d_off" ] && echo MATCH || echo MISMATCH)"
[ "$d" = "$d_off" ] || fail "csv auto digest"

say "--- FIXED/BINARY: serial vs parallel ---"
$P -c "DROP TABLE IF EXISTS public.mvf; CREATE TABLE public.mvf(c1 text, c2 int, c3 bigint, c4 float8, c5 text);" >>"$OUT" 2>&1
cf_off=$(run_fixed off "PARALLEL=OFF")
df_off=$($P -c "$(printf "$DIGEST" public.mvf)" | tail -1)
say "  off   loaded=$cf_off digest=$df_off"
for w in 4 8; do
  c=$(run_fixed w$w "PARALLEL=ON" "PARALLEL_WORKERS=$w")
  d=$($P -c "$(printf "$DIGEST" public.mvf)" | tail -1)
  say "  w$w    loaded=$c digest=$d $([ "$d" = "$df_off" ] && echo MATCH || echo MISMATCH)"
  [ "$d" = "$df_off" ] || fail "fixed w$w digest"
  [ "$c" = "$ROWS" ] || fail "fixed w$w count=$c"
done

say "--- downgrade path: FILTER must fall back and still be correct ---"
# FILTER takes one argument per target column and must return the table rowtype.
$P -c "CREATE OR REPLACE FUNCTION public.fl_identity(text, integer, bigint, numeric, text) RETURNS public.mv AS 'SELECT ROW(\$1,\$2,\$3,\$4,\$5)::public.mv' LANGUAGE sql IMMUTABLE;" >>"$OUT" 2>&1
$P -c "TRUNCATE public.mv;" >/dev/null 2>&1
f1=$($P -c "SET client_min_messages=INFO; SELECT count FROM pgfastload.pg_fastload(ARRAY['INPUT=$CSVFILE','OUTPUT=public.mv','TYPE=CSV','WRITER=DIRECT','FILTER=public.fl_identity']);" 2>&1 | tail -1)
d1=$($P -c "$(printf "$DIGEST" public.mv)" | tail -1)
$P -c "TRUNCATE public.mv;" >/dev/null 2>&1
f2=$($P -c "SET client_min_messages=INFO; SELECT count FROM pgfastload.pg_fastload(ARRAY['INPUT=$CSVFILE','OUTPUT=public.mv','TYPE=CSV','WRITER=DIRECT','LOGFILE=$DATA/fl_filter.log','FILTER=public.fl_identity','PARALLEL=ON']);" 2>&1 | tail -1)
d2=$($P -c "$(printf "$DIGEST" public.mv)" | tail -1)
say "  serial-filter loaded=$f1 digest=$d1"
say "  parallel-filter loaded=$f2 digest=$d2 $([ "$d2" = "$d1" ] && echo MATCH || echo MISMATCH)"
[ "$d1" = "$d2" ] || fail "FILTER downgrade digest"
[ "$f1" = "$ROWS" ] || fail "FILTER selected $f1 rows, expected $ROWS"
if grep -q "PARALLEL disabled (FILTER" "$DATA/fl_filter.log" 2>/dev/null; then
  say "  downgrade notice present in LOGFILE: yes"
else
  fail "FILTER downgrade notice missing from LOGFILE"
fi

say "--- negative: parse-error handling (PARSE_ERRORS semantics) ---"
# 3 records, the middle one is not parseable as (int, int).
printf '1,2\n3,notanint\n5,6\n' > "$DATA/fl_bad.csv"
$P -c "DROP TABLE IF EXISTS public.mvb; CREATE TABLE public.mvb(x int, y int);" >>"$OUT" 2>&1
$P -c "SELECT count FROM pgfastload.pg_fastload(ARRAY['INPUT=$DATA/fl_bad.csv','OUTPUT=public.mvb','TYPE=CSV','WRITER=DIRECT','PARALLEL=OFF']);" >>"$OUT" 2>&1
r_def=$($P -c "SELECT count(*) FROM public.mvb;" | tail -1)
say "  default PARSE_ERRORS=0 -> rows=$r_def (stops at the first bad record, keeps loaded rows)"
[ "$r_def" = "1" ] || fail "default parse-error handling (expected 1 row, got $r_def)"

$P -c "TRUNCATE public.mvb;" >>"$OUT" 2>&1
$P -c "SELECT count FROM pgfastload.pg_fastload(ARRAY['INPUT=$DATA/fl_bad.csv','OUTPUT=public.mvb','TYPE=CSV','WRITER=DIRECT','PARALLEL=OFF','PARSE_ERRORS=2']);" >>"$OUT" 2>&1
r_tol=$($P -c "SELECT count(*) FROM public.mvb;" | tail -1)
say "  PARSE_ERRORS=2 -> rows=$r_tol (bad record rejected, both good rows loaded)"
[ "$r_tol" = "2" ] || fail "tolerant parse-error handling (expected 2 rows, got $r_tol)"

say "--- CLI client ($PG/bin/pg_fastload) ---"
cat > "$DATA/fl_cli.ctl" <<EOF
INPUT = $CSVFILE
OUTPUT = public.mv
TYPE = CSV
WRITER = DIRECT
EOF
$P -c "TRUNCATE public.mv;" >/dev/null 2>&1
$PG/bin/pg_fastload -h "$SOCK" -p "$PORT" -U postgres -d tpcc "$DATA/fl_cli.ctl" >>"$OUT" 2>&1
cli_rc=$?
cli_rows=$($P -c "SELECT count(*) FROM public.mv;" | tail -1)
say "  cli rc=$cli_rc rows=$cli_rows"
[ "$cli_rc" = "0" ] || fail "cli exit code"
[ "$cli_rows" = "$ROWS" ] || fail "cli loaded $cli_rows rows"

say "--- server log check ---"
if grep -qE "PANIC|segmentation|Segmentation" "$DATA/server.log"; then
  fail "panic/segfault in server log"
fi
say "no panic/segfault in server log"

"$PG/bin/pg_ctl" -D "$DATA" -m fast stop >>"$OUT" 2>&1
say "RESULT_${M}=PASS"
