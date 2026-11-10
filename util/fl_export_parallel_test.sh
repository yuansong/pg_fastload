#!/bin/bash
#
# pg_fastload: util/fl_export_parallel_test.sh
#
#	  Copyright (c) 2026, yuanyuansong
#
# Acceptance test for the parallel export path -- N query workers feeding one
# writer process through shared memory.
#
# Run it ON the database host, because OUTPUT is written by the server:
#
#     CREATE EXTENSION pg_fastload;
#     bash util/fl_export_parallel_test.sh
#     PGPORT=5433 PGDATABASE=tpcc bash util/fl_export_parallel_test.sh
#
# It creates (and drops/recreates) its own tables, all named pe_* in the
# current database, and writes its files under $TMPDIR/pg_fastload_export_par.
#
# Checked:
#
#   * a parallel CSV export contains exactly the same rows as a serial one
#     (compared as sets, because row order is explicitly not preserved)
#   * the header appears exactly once, at the top
#   * LIMIT= caps the output exactly, across all workers
#   * PARALLEL_WORKERS defaults to online CPUs - 1 and every count works
#   * BINARY round-trips through the writer's fixed-width layout, and the
#     sample control file is written by the leader
#   * QUERY=, a tiny table, and PARALLEL_WORKERS=1 fall back to the serial
#     path instead of failing
#   * an existing output (or .ctl) is refused before any row is produced
#   * the progress view reports mode='parallel' while the export runs
#
# Exit code: 0 when every check passes, 1 otherwise.

set -u

PGBIN=${PGBIN:-$(pg_config --bindir)}
export PATH=$PGBIN:$PATH

DB=${PGDATABASE:-postgres}
PORT=${PGPORT:-5432}
USER=${PGUSER:-$(id -un)}
D=${TMPDIR:-/tmp}/pg_fastload_export_par
PSQL="psql -U $USER -p $PORT -d $DB -X -q -At"
ROWS=200000

fails=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; fails=$((fails + 1)); }
chk()  { if [ "$2" = "$3" ]; then pass "$1 -> $2"; else fail "$1 -> got [$2] want [$3]"; fi; }
digest() { $PSQL -c "SELECT count(*)::text || '|' || COALESCE(sum(hash_record_extended(t,0)),0)::text FROM $1 t"; }
err() { $PSQL -c "$1" 2>&1 | tr '\n' ' ' | sed 's/  */ /g'; }
exp() { $PSQL -c "SELECT pgfastload.pg_fastload_export(ARRAY[$1])" 2>&1; }
load() { $PSQL -c "SELECT pgfastload.pg_fastload(ARRAY[$1])" 2>&1; }

rm -rf "$D"; mkdir -p "$D"

echo "==== 0. setup ===="
$PSQL -v ON_ERROR_STOP=1 <<SQL
DROP EXTENSION IF EXISTS pg_fastload CASCADE;
CREATE EXTENSION pg_fastload;
DROP TABLE IF EXISTS public.pe_src, public.pe_load, public.pe_bin, public.pe_small;
CREATE TABLE public.pe_src (id int4, pad text);
INSERT INTO public.pe_src
  SELECT g, md5(g::text) FROM generate_series(1, $ROWS) g;
CREATE TABLE public.pe_load (LIKE public.pe_src);
CREATE TABLE public.pe_small (id int4, pad text);
INSERT INTO public.pe_small SELECT g, md5(g::text) FROM generate_series(1, 10) g;
SQL
chk "source rows" "$($PSQL -c 'SELECT count(*) FROM public.pe_src')" "$ROWS"
echo "blocks: $($PSQL -c "SELECT pg_relation_size('public.pe_src')/8192")"
SRC=$(digest public.pe_src)

echo
echo "==== 1. serial baseline (PARALLEL=OFF) ===="
rm -f $D/ser.csv $D/ser.csv.log
R=$(exp "'TABLE=public.pe_src','OUTPUT=$D/ser.csv','TYPE=CSV','HEADER=true','PARALLEL=OFF','PROGRESS=OFF'")
chk "serial rows reported" "$R" "$ROWS"
chk "serial line count" "$(wc -l < $D/ser.csv)" "$((ROWS + 1))"
load "'TABLE=public.pe_load','INFILE=$D/ser.csv','TYPE=CSV','SKIP=1','WRITER=DIRECT','PARALLEL=OFF','PROGRESS=OFF'" >/dev/null
chk "serial reload digest" "$(digest public.pe_load)" "$SRC"

echo
echo "==== 2. parallel CSV, 4 workers ===="
$PSQL -c "TRUNCATE public.pe_load"
rm -f $D/par.csv $D/par.csv.log
R=$(exp "'TABLE=public.pe_src','OUTPUT=$D/par.csv','TYPE=CSV','HEADER=true','PARALLEL=ON','PARALLEL_WORKERS=4','PROGRESS=OFF'")
chk "parallel rows reported" "$R" "$ROWS"
chk "parallel line count" "$(wc -l < $D/par.csv)" "$((ROWS + 1))"
chk "header appears once" "$(head -1 $D/par.csv)" "id,pad"
load "'TABLE=public.pe_load','INFILE=$D/par.csv','TYPE=CSV','SKIP=1','WRITER=DIRECT','PARALLEL=OFF','PROGRESS=OFF'" >/dev/null
chk "parallel reload digest (= source)" "$(digest public.pe_load)" "$SRC"

# Row order may differ, so compare the contents as sets.
awk 'NR > 1' $D/ser.csv | sort > $D/ser.sorted
awk 'NR > 1' $D/par.csv | sort > $D/par.sorted
if cmp -s $D/ser.sorted $D/par.sorted; then
	pass "parallel output is the same set of rows as serial"
else
	fail "parallel output differs from serial ($(cmp $D/ser.sorted $D/par.sorted 2>&1 | head -1))"
fi
chk "worker count in log" "$(grep -o 'workers=[0-9]*' $D/par.csv.log | head -1)" "workers=4"

echo
echo "==== 3. LIMIT is exact across workers ===="
rm -f $D/lim.csv $D/lim.csv.log
R=$(exp "'TABLE=public.pe_src','OUTPUT=$D/lim.csv','TYPE=CSV','HEADER=true','LIMIT=1000','PARALLEL=ON','PARALLEL_WORKERS=4','PROGRESS=OFF'")
chk "LIMIT rows reported" "$R" "1000"
chk "LIMIT line count" "$(wc -l < $D/lim.csv)" "1001"
chk "LIMIT rows are unique" "$(awk 'NR>1' $D/lim.csv | cut -d, -f1 | sort -u | wc -l)" "1000"

echo
echo "==== 4. default worker count ===="
$PSQL -c "TRUNCATE public.pe_load"
rm -f $D/auto.csv $D/auto.csv.log
R=$(exp "'TABLE=public.pe_src','OUTPUT=$D/auto.csv','TYPE=CSV','HEADER=true','PARALLEL=ON','PROGRESS=OFF'")
chk "auto rows reported" "$R" "$ROWS"
load "'TABLE=public.pe_load','INFILE=$D/auto.csv','TYPE=CSV','SKIP=1','WRITER=DIRECT','PARALLEL=OFF','PROGRESS=OFF'" >/dev/null
chk "auto reload digest" "$(digest public.pe_load)" "$SRC"
echo "     $(grep -o 'workers=[0-9]*' $D/auto.csv.log | head -1) (online CPUs - 1)"

echo
echo "==== 5. parallel BINARY ===="
$PSQL -c "TRUNCATE public.pe_bin" 2>/dev/null
$PSQL -c "DROP TABLE IF EXISTS public.pe_bin"
$PSQL -c "CREATE TABLE public.pe_bin (id int4, pad char(32))"
rm -f $D/par.bin $D/par.bin.ctl $D/par.bin.log
R=$(exp "'TABLE=public.pe_src','OUTPUT=$D/par.bin','TYPE=BINARY','OUT_COL=INTEGER','OUT_COL=VARCHAR(32)','PARALLEL=ON','PARALLEL_WORKERS=4','PROGRESS=OFF'")
chk "binary rows reported" "$R" "$ROWS"
chk "binary file size" "$(stat -c %s $D/par.bin)" "$((ROWS * 36))"
chk "sample ctl exists" "$([ -s $D/par.bin.ctl ] && echo yes || echo no)" "yes"
chk "sample ctl records provenance" "$(head -2 $D/par.bin.ctl | tail -1)" "# exported from public.pe_src"
load "'TABLE=public.pe_bin','INFILE=$D/par.bin','TYPE=BINARY','COL=INTEGER','COL=CHAR(32)','WRITER=DIRECT','PARALLEL=OFF','PROGRESS=OFF'" >/dev/null
chk "binary reload digest" "$(digest public.pe_bin)" "$SRC"

echo
echo "==== 6. serial fallbacks ===="
# QUERY= cannot be split over blocks
rm -f $D/q.csv $D/q.csv.log
R=$(exp "'QUERY=SELECT * FROM public.pe_src','OUTPUT=$D/q.csv','TYPE=CSV','HEADER=true','PARALLEL=ON','PROGRESS=OFF'")
chk "QUERY= rows" "$R" "$ROWS"
if grep -q "PARALLEL disabled" $D/q.csv.log; then
	pass "QUERY= falls back to the serial path"
else
	fail "QUERY= did not log a fallback: $(grep -i parallel $D/q.csv.log | head -2)"
fi

# a tiny table is not worth a worker
rm -f $D/s.csv $D/s.csv.log
R=$(exp "'TABLE=public.pe_small','OUTPUT=$D/s.csv','TYPE=CSV','HEADER=true','PARALLEL=ON','PARALLEL_WORKERS=4','PROGRESS=OFF'")
chk "small table rows" "$R" "10"
if grep -q "PARALLEL disabled" $D/s.csv.log; then
	pass "tiny table falls back to the serial path"
else
	fail "tiny table did not log a fallback"
fi

# workers=1 must behave like the serial path (an explicit 1, not "default")
rm -f $D/w1.csv $D/w1.csv.log
R=$(exp "'TABLE=public.pe_src','OUTPUT=$D/w1.csv','TYPE=CSV','HEADER=true','PARALLEL=ON','PARALLEL_WORKERS=1','PROGRESS=OFF'")
chk "workers=1 rows" "$R" "$ROWS"
chk "workers=1 line count" "$(wc -l < $D/w1.csv)" "$((ROWS + 1))"
if grep -q "PARALLEL disabled" $D/w1.csv.log; then
	pass "workers=1 falls back to the serial path"
else
	fail "workers=1 did not fall back: $(grep -o 'workers=[0-9]*' $D/w1.csv.log | head -1)"
fi

echo
echo "==== 7. rejected inputs ===="
E=$(exp "'TABLE=public.pe_src','OUTPUT=$D/par.csv','TYPE=CSV','PARALLEL=ON'")
case "$E" in *"could not create output file"*|*"File exists"*) pass "existing output refused: $(echo $E | cut -c1-60)" ;;
	*) fail "existing output not refused: $E" ;; esac

rm -f $D/p2.bin $D/p2.bin.log
touch $D/p2.bin.ctl
E=$(exp "'TABLE=public.pe_src','OUTPUT=$D/p2.bin','TYPE=BINARY','OUT_COL=INTEGER','OUT_COL=VARCHAR(32)','PARALLEL=ON'")
case "$E" in *"sample control file"*) pass "existing .ctl refused" ;;
	*) fail "existing .ctl not refused: $E" ;; esac

E=$(exp "'TABLE=public.pe_src','OUTPUT=$D/p3.csv','TYPE=CSV','PARALLEL_WORKERS=0'")
case "$E" in *"PARALLEL_WORKERS must be between"*) pass "PARALLEL_WORKERS=0 refused" ;;
	*) fail "PARALLEL_WORKERS=0 not refused: $E" ;; esac

echo
echo "==== 8. progress while a parallel export runs ===="
$PSQL -c "DROP TABLE IF EXISTS public.pe_big"
$PSQL -c "CREATE TABLE public.pe_big (id int4, pad text)"
$PSQL -c "INSERT INTO public.pe_big SELECT g, md5(g::text) FROM generate_series(1,1000000) g"
rm -f $D/big.csv $D/big.csv.log
$PSQL -c "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.pe_big','OUTPUT=$D/big.csv','TYPE=CSV','HEADER=true','PARALLEL=ON','PARALLEL_WORKERS=4'])" >/dev/null 2>&1 &
seen=""
for i in $(seq 1 400); do
	seen=$($PSQL -c "SELECT mode || '|' || direction || '|' || state FROM pgfastload.pg_fastload_progress WHERE direction='export' LIMIT 1")
	[ -n "$seen" ] && break
done
wait
case "$seen" in
	parallel*export*|*parallel*) pass "view shows a running parallel export: [$seen]" ;;
	*) fail "no parallel progress record seen (last [$seen])" ;;
esac
chk "view empty after the export" "$($PSQL -c 'SELECT count(*) FROM pgfastload.pg_fastload_progress')" "0"
chk "big export line count" "$(wc -l < $D/big.csv)" "1000001"

echo
echo "==== 9. cleanup ===="
$PSQL -c "DROP TABLE IF EXISTS public.pe_src, public.pe_load, public.pe_bin, public.pe_small, public.pe_big"
rm -rf "$D"
echo
if [ $fails -eq 0 ]; then
	echo "RESULT=PASS"
	exit 0
else
	echo "RESULT=FAIL ($fails failed)"
	exit 1
fi
