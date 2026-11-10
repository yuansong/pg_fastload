#!/bin/bash
#
# pg_fastload: util/fl_export_test.sh
#
#	  Copyright (c) 2026, yuanyuansong
#
# Acceptance test for pgfastload.pg_fastload_export() -- table/query to
# CSV / BINARY / FIXED.
#
# Run it ON the database host, because OUTPUT is written by the server:
#
#     CREATE EXTENSION pg_fastload;
#     bash util/fl_export_test.sh            # PG* env vars are honoured
#     PGPORT=5433 bash util/fl_export_test.sh
#
# It creates (and drops/recreates) its own tables, all named exp_* in the
# current database, and writes its files under $TMPDIR/pg_fastload_export.
#
# Checked:
#
#   * CSV export is byte-identical to COPY ... TO ... (FORMAT csv, HEADER)
#   * CSV round trip (header skipped) yields the same content digest
#   * a custom dialect (delimiter, quote, NULL) round-trips as well
#   * QUERY= honours WHERE / ORDER BY, LIMIT= caps the output
#   * an empty table produces an empty file
#   * BINARY: record width, digest round trip, sample control file, and the
#     documented VARCHAR-vs-CHAR padding asymmetry
#   * FIXED: text-only layout round-trips
#   * NULLIF keeps NULL in a fixed-width record
#   * 11 rejected inputs (missing OUTPUT, relative path, bad OUT_COL, ...)
#   * progress is published as direction='export' and disappears afterwards
#
# Exit code: 0 when every check passes, 1 otherwise.

set -u

PGBIN=${PGBIN:-$(pg_config --bindir)}
export PATH=$PGBIN:$PATH

DB=${PGDATABASE:-postgres}
PORT=${PGPORT:-5432}
USER=${PGUSER:-$(id -un)}
D=${TMPDIR:-/tmp}/pg_fastload_export
PSQL="psql -U $USER -p $PORT -d $DB -X -q -At"

fails=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; fails=$((fails + 1)); }
chk()  { if [ "$2" = "$3" ]; then pass "$1 -> $2"; else fail "$1 -> got [$2] want [$3]"; fi; }
digest() { $PSQL -c "SELECT count(*)::text || '|' || COALESCE(sum(hash_record_extended(t,0)),0)::text FROM $1 t"; }
err() { $PSQL -c "$1" 2>&1 | tr '\n' ' ' | sed 's/  */ /g'; }

rm -rf "$D"; mkdir -p "$D"

echo "==== 0. setup ===="
$PSQL -v ON_ERROR_STOP=1 <<'SQL'
DROP EXTENSION IF EXISTS pg_fastload CASCADE;
CREATE EXTENSION pg_fastload;
DROP TABLE IF EXISTS public.exp_t, public.exp_t2, public.exp_bin, public.exp_bin2,
                     public.exp_fix2, public.exp_empty, public.exp_null, public.exp_null2;
CREATE TABLE public.exp_t (
  id int4, big int8, amt numeric(10,2), name text,
  fixed varchar(8), ts timestamp, flag bool, note text);
INSERT INTO public.exp_t VALUES
 (1, 9007199254740993, 12.34, 'plain', 'abc', '2026-01-02 03:04:05', true, NULL),
 (2, -1, -0.01, 'has,comma', 'abcdefgh', '2026-11-10 00:00:00', false, ''),
 (3, 0, 0.00, 'has"quote', '', NULL, NULL, E'line1\nline2'),
 (4, NULL, NULL, NULL, NULL, NULL, NULL, '  spaces  '),
 (5, 42, 99.99, E'tab\tinside', 'x', '1970-01-01 00:00:00+00', true, '\N');
CREATE TABLE public.exp_t2 (LIKE public.exp_t);
CREATE TABLE public.exp_bin (
  i2 int2, i4 int4, i8 int8, f4 real, f8 double precision,
  c10 char(10), v8 varchar(8));
INSERT INTO public.exp_bin VALUES
 (1, 2, 3, 1.5, 2.25, 'abc', 'xy'),
 (-32768, -2147483648, -9223372036854775808, -0.5, 1e-300, '0123456789', '12345678'),
 (32767, 2147483647, 9223372036854775807, 3.4e38, 1.7e308, '', ''),
 (0, 0, 0, 0, 0, ' spaced ', 'null');
CREATE TABLE public.exp_bin2 (LIKE public.exp_bin);
CREATE TABLE public.exp_fix2 (LIKE public.exp_bin);
CREATE TABLE public.exp_empty (LIKE public.exp_t);
CREATE TABLE public.exp_null (a int4, c char(4));
INSERT INTO public.exp_null VALUES (1, 'ab'), (NULL, NULL), (3, 'xy');
CREATE TABLE public.exp_null2 (LIKE public.exp_null);
SQL
echo "setup rc=$?"

echo
echo "==== 1. CSV export equals COPY TO ... (FORMAT csv, HEADER) ===="
rm -f $D/copy.csv $D/exp.csv $D/exp.csv.log
$PSQL -c "\\copy (SELECT * FROM public.exp_t) TO '$D/copy.csv' WITH (FORMAT csv, HEADER)"
$PSQL -c "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.exp_t','OUTPUT=$D/exp.csv','TYPE=CSV','HEADER=ON'])" > $D/e1.txt
chk "export returns row count" "$(cat $D/e1.txt)" "5"
if cmp -s $D/copy.csv $D/exp.csv; then pass "exported CSV is byte-identical to COPY"; else fail "CSV differs from COPY"; diff <(cat -A $D/copy.csv) <(cat -A $D/exp.csv) | head -10; fi

echo
echo "==== 2. CSV round trip (header skipped) ===="
T0=$(digest public.exp_t)
$PSQL -c "SELECT count FROM pgfastload.pg_fastload(ARRAY['INPUT=$D/exp.csv','OUTPUT=public.exp_t2','TYPE=CSV','SKIP=1','TRUNCATE=YES'])" > $D/e2.txt
chk "csv load count" "$(cat $D/e2.txt)" "5"
chk "csv round trip digest" "$(digest public.exp_t2)" "$T0"

echo
echo "==== 3. CSV dialect: delimiter | quote ' null \\N ===="
rm -f $D/pipe.csv $D/pipe.csv.log
$PSQL -c "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.exp_t','OUTPUT=$D/pipe.csv','TYPE=CSV','DELIMITER=|','QUOTE=''','NULL=\N'])" > /dev/null
echo "--- pipe file ---"; cat -A $D/pipe.csv
$PSQL -c "TRUNCATE public.exp_t2" > /dev/null
$PSQL -c "SELECT count FROM pgfastload.pg_fastload(ARRAY['INPUT=$D/pipe.csv','OUTPUT=public.exp_t2','TYPE=CSV','DELIMITER=|','QUOTE=''','NULL=\N'])" > $D/e3.txt
chk "pipe load count" "$(cat $D/e3.txt)" "5"
chk "pipe round trip digest" "$(digest public.exp_t2)" "$T0"

echo
echo "==== 4. QUERY= with WHERE / ORDER BY, and LIMIT= ===="
rm -f $D/q.csv $D/q.csv.log
$PSQL -c "SELECT pgfastload.pg_fastload_export(ARRAY['QUERY=SELECT id, name FROM public.exp_t WHERE id >= 3 ORDER BY id DESC','OUTPUT=$D/q.csv','TYPE=CSV','HEADER=ON'])" > $D/e4.txt
chk "query export count" "$(cat $D/e4.txt)" "3"
echo "--- query file ---"; cat -A $D/q.csv
chk "query export first id" "$(sed -n 2p $D/q.csv | cut -d, -f1)" "5"

rm -f $D/l.csv $D/l.csv.log
$PSQL -c "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.exp_t','OUTPUT=$D/l.csv','TYPE=CSV','LIMIT=2'])" > $D/e5.txt
chk "LIMIT=2 count" "$(cat $D/e5.txt)" "2"
chk "LIMIT=2 lines" "$(wc -l < $D/l.csv | tr -d ' ')" "2"

echo
echo "==== 5. empty table ===="
rm -f $D/empty.csv $D/empty.csv.log
$PSQL -c "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.exp_empty','OUTPUT=$D/empty.csv','TYPE=CSV'])" > $D/e6.txt
chk "empty export count" "$(cat $D/e6.txt)" "0"
chk "empty file size" "$(stat -c %s $D/empty.csv)" "0"

echo
echo "==== 6. BINARY export + re-import ===="
rm -f $D/b.bin $D/b.bin.ctl $D/b.bin.log
$PSQL -v ON_ERROR_STOP=1 -c "SELECT pgfastload.pg_fastload_export(ARRAY[
  'TABLE=public.exp_bin','OUTPUT=$D/b.bin','TYPE=BINARY',
  'OUT_COL=SMALLINT','OUT_COL=INTEGER','OUT_COL=BIGINT',
  'OUT_COL=FLOAT','OUT_COL=DOUBLE','OUT_COL=CHAR(10)','OUT_COL=VARCHAR(8)'])" > $D/e7.txt
chk "binary export count" "$(cat $D/e7.txt)" "4"
# 7 columns: 2+4+8+4+8+10+8 = 44 bytes/record * 4 rows = 176
chk "binary record size" "$(stat -c %s $D/b.bin)" "176"
echo "--- sample control file ---"; cat $D/b.bin.ctl
B0=$(digest public.exp_bin)

# The sample names its origin as a comment and keeps OUTPUT = file name, so a
# careless "pg_fastload b.bin.ctl" cannot append the rows back into the source.
chk "sample ctl records the source" "$(grep '^# exported from' $D/b.bin.ctl)" "# exported from public.exp_bin"
chk "sample ctl OUTPUT is the file name" "$(grep '^OUTPUT' $D/b.bin.ctl)" "OUTPUT = b"

# Reload with CHAR(8) for the last column: the fixed-width format space-pads
# every field, and Read_char trims the padding back (Read_varchar would keep it).
$PSQL -c "SELECT count FROM pgfastload.pg_fastload(ARRAY['INPUT=$D/b.bin','OUTPUT=public.exp_bin2','TYPE=BINARY',
  'COL=SMALLINT','COL=INTEGER','COL=BIGINT','COL=FLOAT','COL=DOUBLE','COL=CHAR(10)','COL=CHAR(8)','TRUNCATE=YES'])" > $D/e8.txt
chk "binary load count" "$(cat $D/e8.txt)" "4"
chk "binary round trip digest" "$(digest public.exp_bin2)" "$B0"

# Same file, VARCHAR(8) as declared by OUT_COL: the padding comes back.
$PSQL -c "SELECT count FROM pgfastload.pg_fastload(ARRAY['INPUT=$D/b.bin','OUTPUT=public.exp_bin2','TYPE=BINARY',
  'COL=SMALLINT','COL=INTEGER','COL=BIGINT','COL=FLOAT','COL=DOUBLE','COL=CHAR(10)','COL=VARCHAR(8)','TRUNCATE=YES'])" > /dev/null
chk "VARCHAR reload keeps padding" "$($PSQL -c "SELECT v8 || '|' || length(v8) FROM public.exp_bin2 WHERE i2 = 1")" "xy      |8"

echo
echo "==== 7. FIXED export (text only) + re-import ===="
rm -f $D/f.txt $D/f.txt.ctl $D/f.txt.log
$PSQL -c "SELECT pgfastload.pg_fastload_export(ARRAY[
  'TABLE=public.exp_bin','OUTPUT=$D/f.txt','TYPE=FIXED',
  'OUT_COL=CHAR(6)','OUT_COL=CHAR(11)','OUT_COL=CHAR(20)',
  'OUT_COL=CHAR(15)','OUT_COL=CHAR(24)','OUT_COL=CHAR(10)','OUT_COL=VARCHAR(8)'])" > $D/e9.txt
chk "fixed export count" "$(cat $D/e9.txt)" "4"
echo "--- fixed file (cat -A) ---"; cat -A $D/f.txt
F0=$(digest public.exp_bin)
$PSQL -c "SELECT count FROM pgfastload.pg_fastload(ARRAY['INPUT=$D/f.txt','OUTPUT=public.exp_fix2','TYPE=FIXED',
  'COL=CHAR(6)','COL=CHAR(11)','COL=CHAR(20)','COL=CHAR(15)','COL=CHAR(24)','COL=CHAR(10)','COL=CHAR(8)','TRUNCATE=YES'])" > $D/e10.txt
chk "fixed load count" "$(cat $D/e10.txt)" "4"
chk "fixed round trip digest" "$(digest public.exp_fix2)" "$F0"

echo
echo "==== 8. BINARY NULLIF keeps NULL ===="
rm -f $D/n.bin $D/n.bin.ctl $D/n.bin.log
$PSQL -c "SELECT pgfastload.pg_fastload_export(ARRAY[
  'TABLE=public.exp_null','OUTPUT=$D/n.bin','TYPE=BINARY',
  'OUT_COL=INTEGER NULLIF 00000000','OUT_COL=CHAR(4) NULLIF ''    '''])" > $D/e11.txt
chk "binary with NULLIF count" "$(cat $D/e11.txt)" "3"
N0=$(digest public.exp_null)
$PSQL -c "SELECT count FROM pgfastload.pg_fastload(ARRAY['INPUT=$D/n.bin','OUTPUT=public.exp_null2','TYPE=BINARY',
  'COL=INTEGER NULLIF 00000000','COL=CHAR(4) NULLIF ''    ''','TRUNCATE=YES'])" > $D/e12.txt
chk "binary NULLIF load count" "$(cat $D/e12.txt)" "3"
chk "binary NULLIF round trip digest" "$(digest public.exp_null2)" "$N0"

echo
echo "==== 9. error paths ===="
E1=$(err "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.exp_t','TYPE=CSV'])")
echo "$E1" | grep -q "OUTPUT option required" && pass "missing OUTPUT rejected" || fail "missing OUTPUT: $E1"

E2=$(err "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.exp_t','OUTPUT=rel.csv','TYPE=CSV'])")
echo "$E2" | grep -q "relative path not allowed" && pass "relative OUTPUT rejected" || fail "relative OUTPUT: $E2"

E3=$(err "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.exp_t','OUTPUT=$D/e3.bin','TYPE=BINARY'])")
echo "$E3" | grep -q "requires OUT_COL" && pass "BINARY without OUT_COL rejected" || fail "BINARY no OUT_COL: $E3"

E4=$(err "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.exp_t','OUTPUT=$D/e4.txt','TYPE=FIXED','OUT_COL=INTEGER'])")
echo "$E4" | grep -q "FIXED writes text only" && pass "FIXED with INTEGER rejected" || fail "FIXED numeric: $E4"

E5=$(err "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.exp_t','OUTPUT=$D/e5.bin','TYPE=BINARY','OUT_COL=INTEGER'])")
echo "$E5" | grep -q "OUT_COL declares 1 column" && pass "OUT_COL count mismatch rejected" || fail "count mismatch: $E5"

# The server localises "File exists" (lc_messages), so the assertion is on the
# behaviour instead of the wording: the call must fail and the file must keep
# its original contents.
SIZE6=$(stat -c %s "$D/exp.csv" 2>/dev/null || echo missing)
E6=$(err "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.exp_t','OUTPUT=$D/exp.csv','TYPE=CSV'])")
if [ "$SIZE6" != "missing" ] && echo "$E6" | grep -q "^ERROR" && \
   [ "$(stat -c %s "$D/exp.csv" 2>/dev/null)" = "$SIZE6" ]; then
	pass "existing OUTPUT not overwritten"
else
	fail "existing OUTPUT: $E6"
fi

E7=$(err "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.exp_null','OUTPUT=$D/e7.bin','TYPE=BINARY','OUT_COL=INTEGER','OUT_COL=CHAR(4)'])")
echo "$E7" | grep -q "no representation" && pass "NULL without NULLIF rejected" || fail "NULL without NULLIF: $E7"

E8=$(err "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.exp_t','OUTPUT=$D/e8.csv','TYPE=CSV','BOGUS=1'])")
echo "$E8" | grep -q 'invalid keyword "BOGUS"' && pass "unknown keyword rejected" || fail "unknown keyword: $E8"

E9=$(err "SELECT pgfastload.pg_fastload_export(ARRAY['OUTPUT=$D/e9.csv','TYPE=CSV'])")
echo "$E9" | grep -q "TABLE or QUERY option required" && pass "source required" || fail "source required: $E9"

E10=$(err "SELECT pgfastload.pg_fastload_export(ARRAY['QUERY=UPDATE public.exp_t SET id=id','OUTPUT=$D/e10.csv','TYPE=CSV'])")
echo "$E10" | grep -qi "cannot open UPDATE query as cursor" && pass "non-SELECT QUERY rejected" || fail "non-SELECT: $E10"

E11=$(err "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.exp_t','OUTPUT=$D/e11.csv','TYPE=CSV','DELIMITER=,','QUOTE=,'])")
echo "$E11" | grep -q "must differ" && pass "delimiter == quote rejected" || fail "delimiter==quote: $E11"

echo
echo "==== 10. progress view during an export ===="
$PSQL -c "DROP TABLE IF EXISTS public.exp_big; CREATE TABLE public.exp_big AS SELECT g AS id, md5(g::text) AS pad FROM generate_series(1, 8000000) g" > /dev/null
rm -f $D/big.csv $D/big.csv.log
( $PSQL -c "SELECT pgfastload.pg_fastload_export(ARRAY['TABLE=public.exp_big','OUTPUT=$D/big.csv','TYPE=CSV','PROGRESS_INTERVAL=200'])" > $D/big.out 2>&1 ) &
pid=$!
sleep 0.4
$PSQL -c "SELECT direction || '|' || state || '|' || input_type || '|' || mode || '|' || (rows > 0)::text || '|' || COALESCE(percent::text,'-') || '|' || (bytes_read > 0)::text || '|' || infile FROM pgfastload.pg_fastload_progress"
wait $pid
chk "big export count" "$(cat $D/big.out)" "8000000"
chk "view empty after export" "$($PSQL -c 'SELECT count(*) FROM pgfastload.pg_fastload_progress')" "0"
chk "big csv > 100 MB" "$([ $(stat -c %s $D/big.csv) -gt 100000000 ] && echo yes)" "yes"
echo "lines in exported file: $(wc -l < $D/big.csv)"

echo
if [ $fails -eq 0 ]; then echo "==== done: all checks passed ===="; else echo "==== done: $fails check(s) FAILED ===="; fi
exit $((fails > 0))
