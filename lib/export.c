/*
 * pg_fastload: lib/export.c
 *
 *	  Copyright (c) 2026, yuanyuansong
 */

/**
 * @file
 * @brief Table export: relations and query results to CSV / BINARY / FIXED
 *
 * pg_fastload() copies a file *into* a table.  This module is the other
 * direction, and it is deliberately built from the same two halves: a scan
 * produces tuples, a Writer consumes them.  CSV gets a writer of its own
 * (lib/writer_csv.c); BINARY/FIXED reuse the existing BinaryWriter, so the
 * layout described by OUT_COL is byte-for-byte the layout the loader accepts
 * for COL -- a file exported here loads back with the sample control file that
 * is written next to it.
 *
 * The scan is an SPI cursor, not a heap scan, so that both
 *
 *     TABLE=public.lineitem
 *     QUERY=SELECT id, name FROM public.lineitem WHERE id > 100 ORDER BY id
 *
 * work, and so that the planner, the snapshot, permissions and the row
 * visibility rules are exactly the ones a plain SELECT would use.
 *
 * Progress is published the same way as for a load (see lib/progress.c); the
 * status record says direction=export, and bytes_read then counts bytes
 * written.
 */
#include "pg_fastload.h"

#include <time.h>

#include "access/reloptions.h"
#include "commands/dbcommands.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/portal.h"
#include "utils/timestamp.h"	/* TimestampTzGetDatum (PG 19 no longer pulls it in) */

#include "binary.h"
#include "export.h"
#include "logger.h"
#include "progress.h"
#include "pg_profile.h"
#include "pg_strutil.h"
#include "writer.h"

PG_FUNCTION_INFO_V1(pg_fastload_export);

/**
 * @brief Rows fetched from the cursor per SPI round trip.
 *
 * Large enough to amortise SPI, small enough that a wide row does not pin
 * hundreds of megabytes in the SPI context between two batches.
 */
#define FL_EXPORT_FETCH_ROWS		1000

typedef struct ExportOptions
{
	char   *table;			/**< TABLE=, a relation name */
	char   *query;			/**< QUERY=, a SELECT */
	char   *output;			/**< OUTPUT=, the file to write */
	char   *type;			/**< resolved TYPE: CSV / BINARY / FIXED */
	char   *logfile;		/**< LOGFILE= */
	int64	limit;			/**< LIMIT=, INT64_MAX when unlimited */
	bool	progress;		/**< publish a progress record? */
	bool	progress_notice;	/**< also print NOTICE lines? */
	int		progress_interval;	/**< publish cadence, 0 => default */
	int		noutcol;		/**< number of OUT_COL declarations */
} ExportOptions;

static void ParseExportOptions(Datum options, ExportOptions *eo, Writer **wt);
static void DumpExportParams(ExportOptions *eo, Writer *wt, const char *query);
static void CheckOutCol(const char *value);

static char *timeval_to_cstring(struct timeval tp);

/* ========================================================================
 * Implementation
 * ========================================================================*/

/**
 * @brief Entry point: export a relation or a query result to a file.
 * @return number of rows written.
 */
Datum
pg_fastload_export(PG_FUNCTION_ARGS)
{
	ExportOptions	eo;
	Writer		   *wt = NULL;
	Datum			options;
	MemoryContext	ccxt;
	MemoryContext	rowcxt;
	Portal			portal;
	TupleDesc		desc;
	PGRUsage		ru0;
	PGRUsage		ru1;
	int64			count = 0;
	char		   *query;
	char		   *relname;
	char		   *start;
	char		   *end;
	double			duration;
	bool			binary;
	int				fieldno = -1;

	/* Same rule as pg_fastload(): the output file is written as the server. */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use pg_fastload_export")));

	options = PG_GETARG_DATUM(0);
	ccxt = CurrentMemoryContext;

	pg_rusage_init(&ru0);

	ParseExportOptions(options, &eo, &wt);

	query = eo.query ? eo.query : psprintf("SELECT * FROM %s", eo.table);
	relname = eo.table ? eo.table : "<query>";
	binary = (pg_strcasecmp(eo.type, "CSV") != 0);

	/* The logger has to exist before anything else can fail. */
	CreateLogger(eo.logfile, wt->verbose, false);

	start = timeval_to_cstring(ru0.tv);
	LoggerLog(INFO, "\npg_fastload export %s on %s\n\n",
			  PG_FASTLOAD_VERSION, start);

	DumpExportParams(&eo, wt, query);

	PG_TRY();
	{
		int			ret;

		ProgressInfo	pinfo;

		memset(&pinfo, 0, sizeof(pinfo));
		pinfo.direction = "export";
		pinfo.infile = eo.output;
		pinfo.datname = get_database_name(MyDatabaseId);
		pinfo.relname = relname;
		pinfo.type = eo.type;
		pinfo.mode = "serial";
		pinfo.enabled = eo.progress;
		pinfo.notice = eo.progress_notice;
		pinfo.interval_ms = eo.progress_interval;
		pinfo.limit_rows = eo.limit;

		ret = SPI_connect();
		if (ret != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed: %s", SPI_result_code_string(ret));

		/*
		 * A read-only cursor runs under the caller's snapshot, which is what
		 * a plain SELECT of the same query would see, and it refuses
		 * anything that is not a plain SELECT.
		 */
		portal = SPI_cursor_open_with_args(NULL, query, 0, NULL, NULL, NULL,
										   true, 0);
		if (portal == NULL)
			elog(ERROR, "could not open cursor: %s",
				 SPI_result_code_string(SPI_result));

		desc = portal->tupDesc;
		if (desc == NULL)
			elog(ERROR, "could not determine the result row type");

		if (binary && eo.noutcol != desc->natts)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("OUT_COL declares %d column(s) but the export "
							"produces %d", eo.noutcol, desc->natts)));

		/* CSV formats whatever the query returns, names included. */
		if (!binary)
			wt->desc = desc;

		WriterInit(wt);

		/*
		 * BINARY/FIXED: the target layout comes from OUT_COL, so the source
		 * descriptor has to be handed to the checker explicitly.  Deriving it
		 * from the tuple header instead (what a load does) is not possible
		 * here: a tuple delivered by a cursor may carry no type OID at all.
		 */
		if (binary)
			BinaryWriterSetSourceDesc(wt, desc);

		ProgressInit(&pinfo);

		rowcxt = AllocSetContextCreate(ccxt, "pg_fastload export row",
									   ALLOCSET_DEFAULT_SIZES);

		for (;;)
		{
			long		nrows;
			int			i;

			CHECK_FOR_INTERRUPTS();

			SPI_cursor_fetch(portal, true, FL_EXPORT_FETCH_ROWS);
			nrows = SPI_processed;

			if (nrows == 0)
			{
				SPI_freetuptable(SPI_tuptable);
				break;
			}

			for (i = 0; i < nrows && count < eo.limit; i++)
			{
				HeapTuple	tuple = SPI_tuptable->vals[i];

				MemoryContextSwitchTo(rowcxt);

				/*
				 * BINARY/FIXED: convert the source row to the OUT_COL layout
				 * first (the checker also enforces the column width and the
				 * unsigned ranges), then let the writer lay the bytes out.
				 */
				if (binary)
					tuple = wt->tchecker->checker(wt->tchecker, tuple,
												  &fieldno);

				WriterInsert(wt, tuple);
				wt->count++;
				count++;

				ProgressUpdate(count, wt->bytes);

				MemoryContextSwitchTo(ccxt);
				MemoryContextReset(rowcxt);
			}

			SPI_freetuptable(SPI_tuptable);

			if (count >= eo.limit)
				break;
		}

		ProgressFinish(count, wt->bytes);

		MemoryContextDelete(rowcxt);

		WriterClose(wt, false);
		wt = NULL;

		SPI_finish();
	}
	PG_CATCH();
	{
		ErrorData	   *errdata;
		MemoryContext	ecxt;

		ecxt = MemoryContextSwitchTo(ccxt);
		errdata = CopyErrorData();
		LoggerLog(INFO, "%s\n", errdata->message);
		FreeErrorData(errdata);

		ProgressAbort();

		if (wt)
			WriterClose(wt, true);

		MemoryContextSwitchTo(ecxt);
		PG_RE_THROW();
	}
	PG_END_TRY();

	pg_rusage_init(&ru1);
	duration = (ru1.tv.tv_sec - ru0.tv.tv_sec) +
		(ru1.tv.tv_usec - ru0.tv.tv_usec) / 1000000.0;
	end = timeval_to_cstring(ru1.tv);

	LoggerLog(INFO,
			  "\n  " int64_FMT " Rows exported.\n\n"
			  "Run began on %s\n"
			  "Run ended on %s\n\n"
			  "elapsed %.2f sec\n",
			  count, start, end, duration);

	LoggerClose();

	PG_RETURN_INT64(count);
}

/**
 * @brief Parse the option array into ExportOptions + a Writer.
 *
 * Two passes, like pg_fastload(): the first one picks up the options that
 * decide which writer to build (TYPE), the second one feeds the rest to that
 * writer.  TABLE is consumed here and never reaches the writer, because a
 * writer's TABLE means "output file" in the control-file syntax while here it
 * is the source.
 */
static void
ParseExportOptions(Datum options, ExportOptions *eo, Writer **wt)
{
	List	   *defs;
	List	   *rest_defs = NIL;
	ListCell   *cell;

	memset(eo, 0, sizeof(ExportOptions));
	eo->limit = INT64_MAX;
	eo->progress = true;		/* publish unless asked not to */

	defs = untransformRelOptions(options);

	foreach(cell, defs)
	{
		DefElem	   *opt = lfirst(cell);
		char	   *keyword;
		char	   *value;

		if (opt->arg == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("option \"%s\" has no value", opt->defname)));

		keyword = opt->defname;
		value = strVal(opt->arg);

		if (CompareKeyword(keyword, "TYPE"))
		{
			ASSERT_ONCE(eo->type == NULL);
			eo->type = pstrdup(value);
		}
		else if (CompareKeyword(keyword, "TABLE"))
		{
			ASSERT_ONCE(eo->table == NULL);
			eo->table = pstrdup(value);
		}
		else if (CompareKeyword(keyword, "QUERY"))
		{
			ASSERT_ONCE(eo->query == NULL);
			eo->query = pstrdup(value);
		}
		else if (CompareKeyword(keyword, "LOGFILE"))
		{
			ASSERT_ONCE(eo->logfile == NULL);
			eo->logfile = pstrdup(value);
		}
		else if (CompareKeyword(keyword, "LIMIT"))
		{
			ASSERT_ONCE(eo->limit == INT64_MAX);
			eo->limit = ParseInt64(value, 1);
		}
		else if (CompareKeyword(keyword, "PROGRESS"))
		{
			eo->progress = ParseBoolean(value);
		}
		else if (CompareKeyword(keyword, "PROGRESS_NOTICE"))
		{
			eo->progress_notice = ParseBoolean(value);
		}
		else if (CompareKeyword(keyword, "PROGRESS_INTERVAL"))
		{
			int64	n = ParseInt64(value, 0);

			if (n < FL_PROGRESS_MIN_INTERVAL_MS ||
				n > FL_PROGRESS_MAX_INTERVAL_MS)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("PROGRESS_INTERVAL must be between %d and %d "
								"milliseconds",
								FL_PROGRESS_MIN_INTERVAL_MS,
								FL_PROGRESS_MAX_INTERVAL_MS)));
			eo->progress_interval = (int) n;
			eo->progress = true;
		}
		else if (CompareKeyword(keyword, "OUT_COL"))
		{
			eo->noutcol++;
			rest_defs = lappend(rest_defs, opt);
		}
		else
			rest_defs = lappend(rest_defs, opt);
	}

	/* Default and validation of TYPE. */
	if (eo->type == NULL)
		eo->type = pstrdup("CSV");

	if (pg_strcasecmp(eo->type, "CSV") == 0)
		*wt = CreateCsvWriter(NULL);
	else if (pg_strcasecmp(eo->type, "BINARY") == 0 ||
			 pg_strcasecmp(eo->type, "FIXED") == 0)
		*wt = CreateBinaryWriter(NULL);
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid TYPE \"%s\" for export", eo->type),
				 errhint("Use CSV, BINARY or FIXED.")));

	/* FIXED is text only; the check needs the resolved TYPE, so redo it. */
	if (pg_strcasecmp(eo->type, "FIXED") == 0)
	{
		foreach(cell, rest_defs)
		{
			DefElem	   *opt = lfirst(cell);
			char	   *keyword = opt->defname;

			/*
			 * Verify each declaration before the writer creates the file, so
			 * that a wrong TYPE leaves no half-made output behind.
			 */
			if (CompareKeyword(keyword, "OUT_COL"))
				CheckOutCol(strVal(opt->arg));
		}
	}

	/*
	 * Options the writer owns.  WriterParam() takes OUTPUT, VERBOSE and the
	 * per-format keywords.
	 */
	foreach(cell, rest_defs)
	{
		DefElem	   *opt = lfirst(cell);

		if (!WriterParam(*wt, opt->defname, strVal(opt->arg)))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("invalid keyword \"%s\"", opt->defname)));
	}

	if (eo->table == NULL && eo->query == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("TABLE or QUERY option required")));
	if (eo->table != NULL && eo->query != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("TABLE and QUERY cannot be used together")));

	if ((*wt)->output == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("OUTPUT option required")));

	eo->output = (*wt)->output;

	/* So that the generated sample control file names a real table. */
	(*wt)->sample_output = eo->table;

	if ((*wt)->desc == NULL && pg_strcasecmp(eo->type, "CSV") != 0 &&
		eo->noutcol == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("TYPE=%s requires OUT_COL", eo->type),
				 errhint("Declare one OUT_COL per output column, e.g. "
						 "\"OUT_COL=INTEGER\" or \"OUT_COL=CHAR(16)\".")));

	/* A log line per export, next to the data it describes. */
	if (eo->logfile == NULL)
		eo->logfile = psprintf("%s.log", eo->output);
}

/**
 * @brief Reject an OUT_COL that TYPE=FIXED cannot express.
 *
 * FIXED is the fixed-width *text* format: every field is a padded string.  The
 * numeric COL types are the BINARY format's business.
 */
static void
CheckOutCol(const char *value)
{
	const char *p = value;

	while (isspace((unsigned char) *p))
		p++;

	/* "COL 20" (bare length) is CHAR(20) */
	if (isdigit((unsigned char) *p))
		return;

	if (pg_strncasecmp(p, "CHAR", 4) == 0 ||
		pg_strncasecmp(p, "CHARACTER", 9) == 0 ||
		pg_strncasecmp(p, "VARCHAR", 7) == 0)
		return;

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("TYPE=FIXED writes text only, but OUT_COL %s is not a "
					"character type", value),
			 errhint("Use CHAR(n) or VARCHAR(n), or TYPE=BINARY for native "
					 "numeric fields.")));
}

static void
DumpExportParams(ExportOptions *eo, Writer *wt, const char *query)
{
	StringInfoData	buf;
	char		   *str;

	initStringInfo(&buf);

	str = QuoteString((char *) query);
	appendStringInfo(&buf, "QUERY = %s\n", str);
	pfree(str);

	if (eo->limit == INT64_MAX)
		appendStringInfoString(&buf, "LIMIT = INFINITE\n");
	else
		appendStringInfo(&buf, "LIMIT = " int64_FMT "\n", eo->limit);

	appendStringInfo(&buf, "PROGRESS = %s\n", eo->progress ? "YES" : "NO");
	if (eo->progress)
	{
		appendStringInfo(&buf, "PROGRESS_NOTICE = %s\n",
						 eo->progress_notice ? "YES" : "NO");
		appendStringInfo(&buf, "PROGRESS_INTERVAL = %d\n",
						 eo->progress_interval > 0 ? eo->progress_interval
						 : FL_PROGRESS_DEFAULT_INTERVAL_MS);
	}
	appendStringInfo(&buf, "TYPE = %s\n", eo->type);

	LoggerLog(INFO, buf.data, 0);
	pfree(buf.data);

	WriterDumpParams(wt);
}

static char *
timeval_to_cstring(struct timeval tp)
{
	TimestampTz tz;
	char	   *str;

	tz = (TimestampTz) tp.tv_sec -
		((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);

#ifdef HAVE_INT64_TIMESTAMP
	tz = (tz * USECS_PER_SEC) + tp.tv_usec;
#else
	tz = tz + (tp.tv_usec / 1000000.0);
#endif

	str = DatumGetCString(DirectFunctionCall1(timestamptz_out,
											  TimestampTzGetDatum(tz)));
	return str;
}
