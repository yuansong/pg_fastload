#!/usr/bin/env bash
# fl_boundary_test.sh -- boundary-condition tests for pg_fastload 1.0.0,
# derived clause-by-clause from docs/USER_MANUAL.md.
# Run on the benchmark VM:  bash fl_boundary_test.sh
set -u

export PATH=/home/ysyuan/postgres/release/bin:$PATH
export PGPORT=5433
export PGUSER=postgres
export PGDATABASE=tpcc
export PGOPTIONS='-c client_min_messages=info'
PSQL="psql -X -A -t -q"
TDIR=/tmp/flbtest
RES=$TDIR/result.txt
LOGROOT=$TDIR/logs
mkdir -p "$TDIR" "$LOGROOT"
: > "$RES"

PASS=0; FAIL=0; FAILED=""

note(){ echo "$*" | tee -a "$RES"; }
chk(){
  local id="$1" ok="$2" desc="$3" ev="${4:-}"
  if [ "$ok" -eq 0 ]; then PASS=$((PASS+1)); note "PASS $id $desc ${ev:+| $ev}"
  else FAIL=$((FAIL+1)); FAILED="$FAILED $id"; note "FAIL $id $desc ${ev:+| $ev}"; fi
}
# expect-error helper: call right after o=$(sql ...); r=$?
err_ok(){ [ "$1" -ne 0 ]; }

sql(){ $PSQL -c "$1" 2>&1; }
sqlo(){ $PSQL -o /dev/null -c "$1" 2>/dev/null; }
make_t(){ sqlo "DROP TABLE IF EXISTS $1"; sqlo "CREATE TABLE $1 (id int, t text)"; }

note "== pg_fastload boundary tests v2, $(date '+%F %T') =="
sqlo "CREATE EXTENSION IF NOT EXISTS pg_fastload"

BIG=$TDIR/big.csv
[ -f "$BIG" ] || seq 1 3000000 | awk -v OFS=',' '{print $1,"v"$1}' > "$BIG"

# ============ A. keyword validation ============
make_t flb_a1
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/no_such_file.csv','OUTPUT=public.flb_a1'])"); r=$?
err_ok $r; chk T01 $? 'missing INPUT file -> ERROR' "$(echo "$o" | head -1)"

printf '1,a\n2,b\n' > "$TDIR/a4.csv"
make_t flb_a2
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/a4.csv','OUTPUT=public.flb_a2','BOGUS_OPT=x'])"); r=$?
err_ok $r; chk T02 $? 'unknown keyword BOGUS_OPT rejected' "$(echo "$o" | grep -o 'invalid keyword.*' | head -1)"

make_t flb_a3
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/a4.csv','OUTPUT=public.flb_a3','INPUT=$TDIR/a4.csv'])"); r=$?
err_ok $r; chk T03 $? 'duplicate keyword rejected' "$(echo "$o" | head -1)"

okv=0; ev=""
for v in ON OFF YES NO TRUE FALSE 1 0; do
  make_t flb_a4
  o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/a4.csv','OUTPUT=public.flb_a4','SKIP_INDEX=$v'])"); r=$?
  if [ $r -ne 0 ]; then okv=1; ev="SKIP_INDEX=$v: $(echo "$o"|head -1)"; break; fi
done
chk T04 $okv 'SKIP_INDEX accepts ON/OFF/YES/NO/TRUE/FALSE/1/0' "$ev"

mkw(){ make_t flb_w; sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/a4.csv','OUTPUT=public.flb_w','PARALLEL=ON','PARALLEL_WORKERS=$1'])"; }
o1=$(mkw 0);   r1=$?
o2=$(mkw 65);  r2=$?
o3=$(mkw -1);  r3=$?
[ $r1 -ne 0 ] && [ $r2 -ne 0 ] && [ $r3 -ne 0 ]; chk T05 $? 'PARALLEL_WORKERS 0/65/-1 all ERROR' \
  "$(echo "$o1"|head -1); $(echo "$o2"|head -1)"

make_t flb_w
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/a4.csv','OUTPUT=public.flb_w','PARALLEL=ON','PARALLEL_WORKERS=1','LOGFILE=$LOGROOT/t06.log'])"); r=$?
cnt=$($PSQL -tAc "select count(*) from flb_w")
grep -q 'PARALLEL disabled' "$LOGROOT/t06.log" 2>/dev/null; g=$?
[ $r -eq 0 ] && [ "$cnt" = "2" ] && [ $g -eq 0 ]
chk T06 $? 'PARALLEL_WORKERS=1 -> downgrade INFO + serial success' \
  "rc=$r count=$cnt info=$(grep -o 'PARALLEL disabled[^)]*)' "$LOGROOT/t06.log" 2>/dev/null | head -1)"

o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/a4.csv','OUTPUT=public.flb_a4','TRUNCATE=YES','PROGRESS_INTERVAL=99'])"); r=$?
err_ok $r; chk T07 $? 'PROGRESS_INTERVAL=99 -> ERROR' "$(echo "$o" | head -1)"
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/a4.csv','OUTPUT=public.flb_a4','TRUNCATE=YES','PROGRESS_INTERVAL=3600001'])"); r=$?
err_ok $r; chk T08 $? 'PROGRESS_INTERVAL=3600001 -> ERROR' "$(echo "$o" | head -1)"

printf '1,a\nbad\n2,b\nbad\n3,c\n' > "$TDIR/a9.csv"
make_t flb_a9
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/a9.csv','OUTPUT=public.flb_a9','PARSE_ERRORS=-1','PARALLEL=OFF','LOGFILE=$LOGROOT/t09.log'])"); r=$?
chk T09 $r 'PARSE_ERRORS=-1 accepted (unlimited)' "count=$($PSQL -tAc 'select count(*) from flb_a9' 2>&1)"

sqlo "DROP TABLE IF EXISTS flb_a10"; sqlo "CREATE TABLE flb_a10 (id int primary key, t text)"
sqlo "INSERT INTO flb_a10 VALUES (1,'x')"
printf '1,a\n1,b\n1,c\n' > "$TDIR/a10.csv"
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/a10.csv','OUTPUT=public.flb_a10','DUPLICATE_ERRORS=-1','ON_DUPLICATE_KEEP=NEW','TRUNCATE=NO','PARALLEL=OFF'])"); r=$?
chk T10 $r 'DUPLICATE_ERRORS=-1 accepted (unlimited)' "$(echo "$o" | head -1)"

# ============ B. load semantics ============
make_t flb_b1
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/a4.csv','OUTPUT=public.flb_b1','LIMIT=0'])"); r=$?
cnt=$($PSQL -tAc "select count(*) from flb_b1")
[ $r -eq 0 ] && [ "$cnt" = "0" ]; chk T11 $? 'LIMIT=0 succeeds with 0 rows' "rc=$r count=$cnt"

make_t flb_b2
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/a4.csv','OUTPUT=public.flb_b2','SKIP=100'])"); r=$?
# SKIP beyond EOF raises ERROR "could not skip 100 lines" (verified 2026-11-12; manual 14 updated)
err_ok $r; chk T12 $? 'SKIP beyond EOF -> ERROR (could not skip)' "rc=$r out=$(echo "$o"|head -1)"

# SKIP counts physical lines (manual 6.2): rec1 is one record spanning 2 physical lines.
# SKIP=1 must NOT skip rec1 cleanly (parse error expected); SKIP=2 skips rec1 exactly.
sqlo "DROP TABLE IF EXISTS flb_b3a"; sqlo "DROP TABLE IF EXISTS flb_b3b"
sqlo "CREATE TABLE flb_b3a (id int, t text)"; sqlo "CREATE TABLE flb_b3b (id int, t text)"
printf '"1\n2",3,extra\n4,5\n6,7\n' > "$TDIR/b3.csv"
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/b3.csv','OUTPUT=public.flb_b3a','SKIP=1','PARSE_ERRORS=5','PARALLEL=OFF','LOGFILE=$LOGROOT/t13a.log'])"); r1=$?
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/b3.csv','OUTPUT=public.flb_b3b','SKIP=2','PARALLEL=OFF','LOGFILE=$LOGROOT/t13b.log'])"); r2=$?
c2=$($PSQL -tAc "select count(*) from flb_b3b")
[ $r2 -eq 0 ] && [ "$c2" = "2" ]
chk T13 $? 'SKIP is physical lines: multiline record not skipped by SKIP=1, exactly by SKIP=2' \
  "skip1: rc=$r1 | skip2: rc=$r2 count=$c2"

# quoted empty vs empty field (manual 9.1)
sqlo "DROP TABLE IF EXISTS flb_b4"; sqlo "CREATE TABLE flb_b4 (id int, t text)"
printf '1,\n2,""\n' > "$TDIR/b4.csv"
sqlo "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/b4.csv','OUTPUT=public.flb_b4'])"
r=$($PSQL -tAc "select bool_and((id=1 and t is null) or (id=2 and t='')) from flb_b4" 2>&1)
[ "$r" = "t" ]; chk T14 $? 'empty field -> NULL, quoted "" -> empty string' "all_ok=$r"

# no trailing newline (manual 9.1)
sqlo "DROP TABLE IF EXISTS flb_b5"; sqlo "CREATE TABLE flb_b5 (id int, t text)"
printf '1,a\n2,b' > "$TDIR/b5.csv"
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/b5.csv','OUTPUT=public.flb_b5'])"); r=$?
cnt=$($PSQL -tAc "select count(*) from flb_b5")
[ $r -eq 0 ] && [ "$cnt" = "2" ]; chk T15 $? 'last record without trailing newline loads' "rc=$r count=$cnt"

# parallel + wrong field count -> ERROR, rollback (manual 11)
make_t flb_b6
printf '1,a\n2,b,c\n3,d\n' > "$TDIR/b6.csv"
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/b6.csv','OUTPUT=public.flb_b6','PARALLEL=ON'])"); r=$?
cnt=$($PSQL -tAc "select count(*) from flb_b6")
err_ok $r; e1=$?; [ "$cnt" = "0" ]; e2=$?
[ $e1 -eq 0 ] && [ $e2 -eq 0 ]
chk T16 $? 'parallel: rejected record -> ERROR + full rollback' "rc=$r count=$cnt out=$(echo "$o"|head -1)"

# serial tolerance -> badfile (manual 11). Defaults live in $PGDATA/pg_fastload/
# as {timestamp}_{db}_{nsp}_{tbl}.prs.{ext}, NOT next to a custom LOGFILE.
make_t flb_b7
rm -f /tmp/pgdata/pg_fastload/*_public_flb_b7.prs.*
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/b6.csv','OUTPUT=public.flb_b7','PARALLEL=OFF','PARSE_ERRORS=5','LOGFILE=$LOGROOT/t17.log'])"); r=$?
cnt=$($PSQL -tAc "select count(*) from flb_b7")
prs=$(ls /tmp/pgdata/pg_fastload/*_public_flb_b7.prs.* 2>/dev/null | head -1)
grep -q 'Parse error Record' "$LOGROOT/t17.log" 2>/dev/null; g=$?
[ $r -eq 0 ] && [ "$cnt" = "2" ] && [ -n "$prs" ] && [ $g -eq 0 ]
chk T17 $? 'serial: 2 good rows kept, bad record to badfile + log WARNING' \
  "rc=$r count=$cnt badfile=$prs warn=$g"

# PARSE_ERRORS exceeded -> stop reading, normal return (manual 6.6)
make_t flb_b8
printf '1,a\nbad\nbad\nbad\n4,d\n5,e\n' > "$TDIR/b8.csv"
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/b8.csv','OUTPUT=public.flb_b8','PARSE_ERRORS=2','PARALLEL=OFF','LOGFILE=$LOGROOT/t18.log'])"); r=$?
cnt=$($PSQL -tAc "select count(*) from flb_b8")
grep -q 'Maximum parse error count exceeded' "$LOGROOT/t18.log" 2>/dev/null; g=$?
[ "$cnt" -lt 3 ] && [ $g -eq 0 ]
chk T18 $? 'PARSE_ERRORS=2 stops reading, log line present' "rc=$r loaded=$cnt logline=$g"

# duplicate keys on the parallel path (manual 11)
make_t flb_b9; sqlo "ALTER TABLE flb_b9 ADD UNIQUE (id)"
sqlo "INSERT INTO flb_b9 VALUES (1,'orig')"
printf '1,new\n2,ok\n' > "$TDIR/b9.csv"
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/b9.csv','OUTPUT=public.flb_b9','PARALLEL=ON','PARALLEL_WORKERS=4','DUPLICATE_ERRORS=5','ON_DUPLICATE_KEEP=NEW','LOGFILE=$LOGROOT/t19.log'])"); r=$?
new=$($PSQL -tAc "select t from flb_b9 where id=1")
chk T19 $r 'parallel: duplicate tolerated, KEEP=NEW wins' "rc=$r row1=$new out=$(echo "$o"|head -2|tr '\n' ' ')"

# parallel digest equals serial digest (manual 10.5)
sqlo "DROP TABLE IF EXISTS flb_b10s"; sqlo "DROP TABLE IF EXISTS flb_b10p"
sqlo "CREATE TABLE flb_b10s (id int, t text)"; sqlo "CREATE TABLE flb_b10p (id int, t text)"
head -5000 "$BIG" > "$TDIR/b10.csv"
sqlo "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/b10.csv','OUTPUT=public.flb_b10s','PARALLEL=OFF'])"
sqlo "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/b10.csv','OUTPUT=public.flb_b10p','PARALLEL=ON','PARALLEL_WORKERS=4'])"
d1=$($PSQL -tAc "select count(*)||'|'||sum(hash_record_extended(flb_b10s,0)) from flb_b10s")
d2=$($PSQL -tAc "select count(*)||'|'||sum(hash_record_extended(flb_b10p,0)) from flb_b10p")
[ -n "$d1" ] && [ "$d1" = "$d2" ]; chk T20 $? 'serial/parallel row-multiset digest equal' "digest=$d1"

# ============ C. SKIP_INDEX contract (manual 6.3) ============
sqlo "DROP TABLE IF EXISTS flb_c1"; sqlo "CREATE TABLE flb_c1 (id int, t text)"
sqlo "CREATE INDEX flb_c1_idx ON flb_c1 (t)"
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/a4.csv','OUTPUT=public.flb_c1','TRUNCATE=YES','SKIP_INDEX=ON','LOGFILE=$LOGROOT/t21.log'])"); r=$?
n=$($PSQL -tAc "select count(*) from pg_indexes where tablename='flb_c1'")
grep -q 'CREATE INDEX' "$LOGROOT/t21.log" 2>/dev/null; g=$?
[ $r -eq 0 ] && [ "$n" = "0" ] && [ $g -eq 0 ]
chk T21 $? 'SKIP_INDEX=ON drops plain index + prints CREATE INDEX' "rc=$r idx_left=$n notice=$g"

sqlo "DROP TABLE IF EXISTS flb_c2"; sqlo "CREATE TABLE flb_c2 (id int primary key, t text)"
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/a4.csv','OUTPUT=public.flb_c2','SKIP_INDEX=ON'])"); r=$?
err_ok $r; chk T22 $? 'SKIP_INDEX=ON on constraint index -> ERROR' "rc=$r out=$(echo "$o"|head -1)"

sqlo "DROP TABLE IF EXISTS flb_c3"; sqlo "CREATE TABLE flb_c3 (id int, t text)"
sqlo "CREATE INDEX flb_c3_idx ON flb_c3 (t)"
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/a4.csv','OUTPUT=public.flb_c3','SKIP_INDEX=ON','DUPLICATE_ERRORS=1'])"); r=$?
err_ok $r; chk T23 $? 'SKIP_INDEX + DUP_ERRORS -> ERROR' "rc=$r out=$(echo "$o"|head -1)"

# ============ D. fixed-length ============
sqlo "DROP TABLE IF EXISTS flb_d1"; sqlo "CREATE TABLE flb_d1 (id int, name varchar(20))"
python3 -c "
import struct
open('/tmp/flbtest/d1.bin','wb').write(struct.pack('<i',1)+b'alice'.ljust(20)+struct.pack('<i',2)+b'bob'.ljust(20))
open('/tmp/flbtest/d1part.bin','wb').write(struct.pack('<i',1)+b'alice'.ljust(20)+struct.pack('<i',2)+b'bob'.ljust(20)+b'TAIL')"
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/d1.bin','OUTPUT=public.flb_d1','TYPE=BINARY','COL=INTEGER','COL=VARCHAR(20)'])"); r=$?
cnt=$($PSQL -tAc "select count(*) from flb_d1")
[ $r -eq 0 ] && [ "$cnt" = "2" ]; chk T24 $? 'BINARY load: native int + text' "rc=$r count=$cnt"

# STRIDE <= 0: ERROR or serial downgrade, never parallel (manual 10.4 #10)
sqlo "DROP TABLE IF EXISTS flb_d2"; sqlo "CREATE TABLE flb_d2 (id int, name varchar(20))"
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/d1.bin','OUTPUT=public.flb_d2','TYPE=BINARY','STRIDE=0','COL=INTEGER','COL=VARCHAR(20)','PARALLEL=ON','LOGFILE=$LOGROOT/t25.log'])"); r=$?
if [ $r -eq 0 ]; then
  grep -q 'PARALLEL disabled' "$LOGROOT/t25.log" 2>/dev/null; ok=$?
else ok=0; fi
chk T25 $ok 'STRIDE=0: ERROR or serial downgrade, never parallel' "rc=$r info=$(grep -o 'PARALLEL disabled[^)]*)' "$LOGROOT/t25.log" 2>/dev/null | head -1)"

# NULLIF > 64 bytes -> downgrade (manual 10.4 #11); needs a text field wide
# enough to accept the pattern (VARCHAR(20) would fail the length check first).
sqlo "DROP TABLE IF EXISTS flb_d3"; sqlo "CREATE TABLE flb_d3 (id int, name char(100))"
python3 -c "
import struct
open('/tmp/flbtest/d3.bin','wb').write(struct.pack('<i',1)+b'alice'.ljust(100))"
LONG=$(python3 -c "print('A'*65)")
ELEM="\$PG\$COL=CHAR(100) NULLIF '$LONG'\$PG\$"
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/d3.bin','OUTPUT=public.flb_d3','TYPE=BINARY','PARALLEL=ON','COL=INTEGER',$ELEM,'LOGFILE=$LOGROOT/t26.log'])"); r=$?
info=$(grep -o 'PARALLEL disabled[^)]*)' "$LOGROOT/t26.log" 2>/dev/null | head -1)
[ -n "$info" ] && [ $r -eq 0 ]; chk T26 $? 'NULLIF pattern > 64 bytes -> downgrade + success' "rc=$r info=$info out=$(echo "$o"|head -1)"

# trailing partial record -> WARNING, whole records loaded (manual 9.2)
sqlo "DROP TABLE IF EXISTS flb_d4"; sqlo "CREATE TABLE flb_d4 (id int, name varchar(20))"
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/d1part.bin','OUTPUT=public.flb_d4','TYPE=BINARY','COL=INTEGER','COL=VARCHAR(20)','LOGFILE=$LOGROOT/t27.log'])"); r=$?
cnt=$($PSQL -tAc "select count(*) from flb_d4")
# the WARNING goes to the client (ereport), not to the pg_fastload LOGFILE
echo "$o" | grep -qi 'warning'; g=$?
[ $r -eq 0 ] && [ "$cnt" = "2" ] && [ $g -eq 0 ]
chk T27 $? 'trailing partial record: 2 rows + WARNING' "rc=$r count=$cnt warn=$g out=$(echo "$o"|head -1)"

# export FIXED rejects numeric OUT_COL (manual 18.4)
sqlo "DROP TABLE IF EXISTS flb_d5"; sqlo "CREATE TABLE flb_d5 (id int, v int)"; sqlo "INSERT INTO flb_d5 VALUES (1,2)"
rm -f "$TDIR/d5.out"
o=$(sql "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.flb_d5','OUTPUT=$TDIR/d5.out','TYPE=FIXED','OUT_COL=INTEGER','OUT_COL=INTEGER'])"); r=$?
err_ok $r; chk T28 $? 'export TYPE=FIXED + OUT_COL=INTEGER -> ERROR' "rc=$r out=$(echo "$o"|grep -o 'TYPE=FIXED writes text only[^ ]*' | head -1)"

# NULL without NULLIF in BINARY export (manual 18.5)
sqlo "DROP TABLE IF EXISTS flb_d6"; sqlo "CREATE TABLE flb_d6 (id int, v int)"; sqlo "INSERT INTO flb_d6 VALUES (1,NULL)"
rm -f "$TDIR/d6.out"
o=$(sql "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.flb_d6','OUTPUT=$TDIR/d6.out','TYPE=BINARY','OUT_COL=INTEGER','OUT_COL=INTEGER'])"); r=$?
err_ok $r; chk T29 $? 'export NULL without NULLIF -> explicit ERROR' "rc=$r out=$(echo "$o"|grep -o 'is NULL and OUT_COL[^ ]*' | head -1)"

# BINARY roundtrip digest (manual 18.5)
sqlo "DROP TABLE IF EXISTS flb_d7"; sqlo "CREATE TABLE flb_d7 (id int, v float8)"
sqlo "INSERT INTO flb_d7 SELECT g, g*1.5/7 FROM generate_series(-100,100) g"
rm -f "$TDIR/d7.out" "$TDIR/d7.out.ctl"   # the sample .ctl is also O_EXCL: stale copy fails the export
sqlo "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.flb_d7','OUTPUT=$TDIR/d7.out','TYPE=BINARY','OUT_COL=INTEGER','OUT_COL=DOUBLE','PARALLEL=OFF'])"
sqlo "DROP TABLE flb_d7"; sqlo "CREATE TABLE flb_d7 (id int, v float8)"
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/d7.out','OUTPUT=public.flb_d7','TYPE=BINARY','COL=INTEGER','COL=DOUBLE'])"); r=$?
d1=$($PSQL -tAc "select count(*)||'|'||sum(hash_record_extended(flb_d7,0)) from flb_d7")
d0=$($PSQL -tAc "select count(*)||'|'||sum(hash_record_extended(t,0)) from (SELECT g, (g*1.5/7)::float8 as v FROM generate_series(-100,100) g) t")
[ $r -eq 0 ] && [ "$d1" = "$d0" ]; chk T30 $? 'BINARY export -> load roundtrip digest equal' "rc=$r src=$d0 rt=$d1"

# ============ E. export boundaries ============
sqlo "DROP TABLE IF EXISTS flb_e1"; sqlo "CREATE TABLE flb_e1 (id int, t text)"
head -100 "$BIG" > "$TDIR/e1.csv"
sqlo "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/e1.csv','OUTPUT=public.flb_e1'])"
rm -f "$TDIR/e1.out"
sqlo "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.flb_e1','OUTPUT=$TDIR/e1.out','TYPE=CSV'])"
o=$(sql "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.flb_e1','OUTPUT=$TDIR/e1.out','TYPE=CSV'])"); r=$?
err_ok $r; chk T31 $? 'export OUTPUT exists -> ERROR (O_EXCL)' "rc=$r out=$(echo "$o"|head -1)"

o=$(sql "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.flb_e1','OUTPUT=rel_out.csv','TYPE=CSV'])"); r=$?
err_ok $r; chk T32 $? 'export relative OUTPUT rejected (absolute required)' "rc=$r out=$(echo "$o"|head -1)"

o=$(sql "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.flb_e1','OUTPUT=/dev/null','TYPE=CSV'])"); r=$?
err_ok $r; chk T33 $? 'export to /dev/null -> ERROR' "rc=$r out=$(echo "$o"|head -1)"

o=$(sql "SELECT pgfastload.pg_fastload_export(ARRAY['QUERY=UPDATE flb_e1 SET t=''x''','OUTPUT=$TDIR/e4.out','TYPE=CSV'])"); r=$?
err_ok $r; chk T34 $? 'export QUERY=UPDATE rejected' "rc=$r out=$(echo "$o"|grep -o 'cannot open UPDATE[^ ]*' | head -1)"

rm -f "$TDIR/e5.out"
n=$(sql "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.flb_e1','OUTPUT=$TDIR/e5.out','TYPE=CSV','PARALLEL=ON','LIMIT=1'])"); r=$?
[ $r -eq 0 ] && [ "$n" = "1" ]; chk T35 $? 'parallel export LIMIT=1 exact' "rc=$r rows=$n"

rm -f "$TDIR/e6.out" "$TDIR/e6.out.log"
n=$(sql "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.flb_e1','OUTPUT=$TDIR/e6.out','TYPE=CSV','PARALLEL=ON','LOGFILE=$TDIR/e6.log'])"); r=$?
tot=$($PSQL -tAc "select count(*) from flb_e1")
[ $r -eq 0 ] && [ "$n" = "$tot" ]; chk T36 $? 'parallel export row count == table count' "rc=$r export=$n table=$tot"

# empty table export (manual 18.3)
sqlo "DROP TABLE IF EXISTS flb_e7"; sqlo "CREATE TABLE flb_e7 (id int, t text)"
rm -f "$TDIR/e7h.out" "$TDIR/e7.out"
sqlo "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.flb_e7','OUTPUT=$TDIR/e7h.out','TYPE=CSV','HEADER=ON'])"
sqlo "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.flb_e7','OUTPUT=$TDIR/e7.out','TYPE=CSV'])"
hl=$(wc -l < "$TDIR/e7h.out" 2>/dev/null || echo 9); hs=$(stat -c %s "$TDIR/e7.out" 2>/dev/null || echo 9)
[ "$hl" = "1" ] && [ "$hs" = "0" ]; chk T37 $? 'empty table: HEADER=ON -> 1 line, else 0 bytes' "hdr_lines=$hl bytes=$hs"

# small table -> parallel downgrade (manual 18.6)
rm -f "$TDIR/e8.out" "$TDIR/e8.log"
sqlo "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.flb_e1','OUTPUT=$TDIR/e8.out','TYPE=CSV','PARALLEL=ON','LOGFILE=$TDIR/e8.log'])"; r=$?
grep -q 'PARALLEL disabled' "$TDIR/e8.log" 2>/dev/null; g=$?
[ $g -eq 0 ]; chk T38 $? 'export small table -> PARALLEL disabled INFO' "rc=$r info=$(grep -o 'PARALLEL disabled[^)]*)' "$TDIR/e8.log" 2>/dev/null | head -1)"

# sample .ctl is written by BINARY/FIXED exports (manual 18.4; CSV exports have none)
[ -f "$TDIR/d7.out.ctl" ] && grep -q 'exported from public.flb_d7' "$TDIR/d7.out.ctl" && grep -q 'TYPE = BINARY' "$TDIR/d7.out.ctl"
chk T39 $? 'BINARY export writes sample .ctl with source comment + TYPE = BINARY' \
  "$(head -3 "$TDIR/d7.out.ctl" 2>/dev/null | tr '\n' ' ')"

grep -q 'snapshot: xmin=' "$TDIR/e6.log" 2>/dev/null
chk T40 $? 'export log records snapshot fingerprint' "$(grep -o 'snapshot: xmin=[0-9]*' "$TDIR/e6.log" 2>/dev/null | head -1)"

# ============ F. progress (manual 12.4) ============
sqlo "DROP TABLE IF EXISTS flb_f1"; sqlo "CREATE TABLE flb_f1 (id bigint, t text)"
( $PSQL -o /dev/null -c "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$BIG','OUTPUT=public.flb_f1','PARALLEL=OFF','PROGRESS_INTERVAL=100'])" >/dev/null 2>&1 ) &
maxseen=0
for i in $(seq 1 20); do
  c=$($PSQL -tAc "select count(*) from pgfastload.pg_fastload_progress where state='running'" 2>/dev/null)
  [ "${c:-0}" -ge 1 ] && { maxseen=$c; break; }
  sleep 0.3
done
wait
[ "$maxseen" -ge 1 ]; chk T41 $? 'progress record visible during load; PROGRESS_INTERVAL implies ON' "max_seen=$maxseen"

sqlo "TRUNCATE flb_f1"
( $PSQL -o /dev/null -c "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$BIG','OUTPUT=public.flb_f1','PARALLEL=OFF','PROGRESS=OFF'])" >/dev/null 2>&1 ) &
seen=0
for i in $(seq 1 8); do
  c=$($PSQL -tAc "select count(*) from pgfastload.pg_fastload_progress" 2>/dev/null)
  [ "${c:-0}" -ge 1 ] && { seen=$c; break; }
  sleep 0.4
done
wait
[ "$seen" = "0" ]; chk T42 $? 'PROGRESS=OFF publishes nothing' "seen=$seen"

cols=$($PSQL -tAc "select column_name from information_schema.columns where table_schema='pgfastload' and table_name='pg_fastload_progress' order by ordinal_position" 2>/dev/null | paste -sd,)
fourth=$(echo "$cols" | cut -d, -f4)
[ "$fourth" = "direction" ]; chk T43 $? 'progress view 4th column is direction (manual 18.7)' "cols=$cols"

acl=$($PSQL -tAc "select coalesce(array_to_string(proacl,','),'NULL') from pg_proc p join pg_namespace n on n.oid=p.pronamespace where n.nspname='pgfastload' and proname='fl_progress_rows'")
case "$acl" in
  *"{=X,"*|*",=X,"*|*",=X}"*|"{=X}"*) ok=1 ;;
  ""|"NULL") ok=1 ;;
  *) ok=0 ;;
esac
chk T44 $ok 'fl_progress_rows EXECUTE revoked from PUBLIC' "acl=$acl"

# ============ G. misc (manual 14) ============
sqlo "DROP VIEW IF EXISTS flb_v1; CREATE VIEW flb_v1 AS SELECT 1 id, ''::text t"
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/a4.csv','OUTPUT=public.flb_v1'])"); r=$?
err_ok $r; chk T45 $? 'load to a view -> cannot load ERROR' "rc=$r out=$(echo "$o"|grep -o 'cannot load to[^ ]*'|head -1)"
o=$(sql "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=$TDIR/a4.csv','OUTPUT=public.flb_a1','TYPE=ZIP'])"); r=$?
err_ok $r; chk T46 $? 'invalid TYPE -> ERROR' "rc=$r out=$(echo "$o"|grep -o 'invalid TYPE[^ ]*'|head -1)"

note "== RESULT: PASS=$PASS FAIL=$FAIL =="
[ -n "$FAILED" ] && note "failed:$FAILED"
exit 0
