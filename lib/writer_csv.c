/*
 * pg_fastload: lib/writer_csv.c
 *
 *	  Copyright (c) 2026, yuanyuansong
 */

/**
 * @file
 * @brief CSV file writer
 *
 * The export direction of the extension: tuples arrive from a table scan and
 * are formatted as a CSV file.  Quotes, escapes and the NULL convention follow
 * COPY ... TO ... (FORMAT csv), so a file produced here is read back by
 * TYPE=CSV with exactly the same meaning for every field, including the
 * difference between an empty string and NULL.
 *
 * The writer keeps one large output buffer and flushes it with write(2) at row
 * boundaries; nothing is fsync'd per row.  Unlike the load direction there is
 * no parser to agree with, so the row is built in the buffer directly and each
 * byte is copied exactly once.
 */
#include "pg_fastload.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/heapam.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "logger.h"
#include "pg_profile.h"
#include "pg_strutil.h"
#include "storage/fd.h"
#include "writer.h"

/**
 * @brief Flush the output buffer once it grows past this many bytes.
 */
#define CSV_FLUSH_THRESHOLD		(4 * 1024 * 1024)

typedef struct CsvWriter
{
	Writer	base;

	int		fd;				/**< output file descriptor */
	StringInfoData buf;		/**< output buffer, holds whole rows */
	StringInfo	sink;		/**< non-NULL: append rows here instead of buf,
								 * no file is opened (parallel export) */

	char	delim;			/**< field separator */
	char	quote;			/**< quotation character */
	char	escape;			/**< character prefixed to quote / escape */
	char   *nullstr;		/**< textual representation of NULL */
	int		nullstr_len;	/**< length of nullstr */

	bool	header;			/**< write a header line? */

	int			natts;		/**< number of columns */
	FmgrInfo   *outfuncs;	/**< output function of each column */
	Datum	   *values;		/**< deform target */
	bool	   *nulls;		/**< deform target */
} CsvWriter;

static void	CsvWriterInit(CsvWriter *self);
static void	CsvWriterInsert(CsvWriter *self, HeapTuple tuple);
static WriterResult CsvWriterClose(CsvWriter *self, bool onError);
static bool	CsvWriterParam(CsvWriter *self, const char *keyword, char *value);
static void	CsvWriterDumpParams(CsvWriter *self);

static void csv_open(CsvWriter *self);
static void csv_flush(CsvWriter *self);
static void csv_put_field(CsvWriter *self, const char *str, bool header);
static void csv_setup(CsvWriter *self);
static void csv_header_line(CsvWriter *self, StringInfo out);

/**
 * @brief Where a formatted row goes: the sink when one is installed, else the
 * writer's own file buffer.
 */
static StringInfo
csv_out(CsvWriter *self)
{
	return self->sink ? self->sink : &self->buf;
}

/* ========================================================================
 * Implementation
 * ========================================================================*/

/**
 * @brief Create a new CsvWriter
 */
Writer *
CreateCsvWriter(void *opt)
{
	CsvWriter  *self;

	self = palloc0(sizeof(CsvWriter));
	self->base.init = (WriterInitProc) CsvWriterInit;
	self->base.insert = (WriterInsertProc) CsvWriterInsert;
	self->base.close = (WriterCloseProc) CsvWriterClose;
	self->base.param = (WriterParamProc) CsvWriterParam;
	self->base.dumpParams = (WriterDumpParamsProc) CsvWriterDumpParams;
	self->base.sendQuery = NULL;

	self->fd = -1;
	self->delim = ',';
	self->quote = '"';
	self->escape = '"';
	self->nullstr = pstrdup("");
	self->header = false;

	return (Writer *) self;
}

/**
 * @brief Initialize the CSV writer: validate the dialect, open the file.
 */
static void
CsvWriterInit(CsvWriter *self)
{
	csv_setup(self);

	initStringInfo(&self->buf);
	enlargeStringInfo(&self->buf, 64 * 1024);

	self->base.context = AllocSetContextCreate(CurrentMemoryContext,
											   "CsvWriter",
											   ALLOCSET_DEFAULT_SIZES);

	csv_open(self);

	if (self->header)
	{
		csv_header_line(self, &self->buf);
		csv_flush(self);
	}
}

/**
 * @brief Everything CsvWriterInit() does except touching the output file.
 *
 * Split out for the parallel export, where the row is formatted in a query
 * worker and written by a separate process: the worker needs the dialect, the
 * resolved output functions and the deform buffers, but must not create the
 * file.  Idempotent, because the parallel leader also calls it to build the
 * header line before any worker exists.
 */
static void
csv_setup(CsvWriter *self)
{
	TupleDesc	desc = self->base.desc;
	int			i;
	Oid			outfunc;

	if (self->outfuncs != NULL)
		return;

	if (desc == NULL)
		elog(ERROR, "CSV writer needs a source tuple descriptor");

	/*
	 * A CSV dialect has to be unambiguous before a single row is written,
	 * because a wrong choice is only visible as unusable data much later.
	 */
	if (self->delim == '\n' || self->delim == '\r' ||
		self->quote == '\n' || self->quote == '\r' ||
		self->escape == '\n' || self->escape == '\r')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("DELIMITER, QUOTE and ESCAPE cannot be a newline")));
	if (self->delim == self->quote || self->delim == self->escape)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("DELIMITER must differ from QUOTE and ESCAPE")));
	if (strchr(self->nullstr, self->delim) != NULL ||
		strchr(self->nullstr, self->quote) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("NULL must not contain the DELIMITER or the QUOTE "
						"character")));

	self->nullstr_len = strlen(self->nullstr);

	self->natts = desc->natts;
	self->outfuncs = palloc0(sizeof(FmgrInfo) * self->natts);
	self->values = palloc(sizeof(Datum) * self->natts);
	self->nulls = palloc(sizeof(bool) * self->natts);

	for (i = 0; i < self->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		bool				isvarlena;

		if (att->attisdropped)
			continue;

		getTypeOutputInfo(att->atttypid, &outfunc, &isvarlena);
		fmgr_info(outfunc, &self->outfuncs[i]);
	}
}

/**
 * @brief Render the header line into @a out (row formatting rules apply).
 */
static void
csv_header_line(CsvWriter *self, StringInfo out)
{
	TupleDesc	desc = self->base.desc;
	int			i;

	for (i = 0; i < self->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (i > 0)
			appendStringInfoCharMacro(out, self->delim);
		csv_put_field(self, NameStr(att->attname), true);
	}
	appendStringInfoCharMacro(out, '\n');
}

void
CsvWriterPrepare(Writer *writer)
{
	CsvWriter  *self = (CsvWriter *) writer;

	csv_setup(self);
}

void
CsvWriterSetSink(Writer *writer, StringInfo sink)
{
	CsvWriter  *self = (CsvWriter *) writer;

	self->sink = sink;
}

void
CsvWriterAppendHeader(Writer *writer, StringInfo out)
{
	CsvWriter  *self = (CsvWriter *) writer;
	StringInfo	save = self->sink;

	/* csv_put_field() targets the sink, so point it at the caller's buffer */
	self->sink = out;
	csv_header_line(self, out);
	self->sink = save;
}

/**
 * @brief Open the output file, refusing to overwrite an existing one.
 */
static void
csv_open(CsvWriter *self)
{
#if PG_VERSION_NUM >= 110000
	self->fd = BasicOpenFilePerm(self->base.output,
								 O_WRONLY | O_CREAT | O_EXCL | PG_BINARY,
								 S_IRUSR | S_IWUSR);
#else
	self->fd = BasicOpenFile(self->base.output,
							 O_WRONLY | O_CREAT | O_EXCL | PG_BINARY,
							 S_IRUSR | S_IWUSR);
#endif
	if (self->fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open CSV output file \"%s\": %m",
						self->base.output)));
}

/**
 * @brief Write out the buffered rows.
 */
static void
csv_flush(CsvWriter *self)
{
	int		len;
	int		written;

	if (self->fd < 0 || self->buf.len == 0)
		return;

	len = self->buf.len;
	written = (int) write(self->fd, self->buf.data, len);
	if (written != len)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to CSV output file \"%s\": %m",
						self->base.output)));

	self->base.bytes += len;
	resetStringInfo(&self->buf);
}

/**
 * @brief Append one field, quoted exactly when it has to be.
 *
 * Mirrors CopyAttributeOutCSV(): a field is quoted when it contains the
 * delimiter, the quote, the escape, a newline or a carriage return, when it
 * equals the NULL string (otherwise NULL and that value would be
 * indistinguishable), and when it is empty while NULL is an empty string too.
 */
static void
csv_put_field(CsvWriter *self, const char *str, bool header)
{
	const char *p;
	bool		need_quote = false;
	StringInfo	out = csv_out(self);

	if (*str == '\0')
		need_quote = (header || self->nullstr_len == 0);
	else if (!header && strcmp(str, self->nullstr) == 0)
		need_quote = true;

	if (!need_quote)
	{
		for (p = str; *p != '\0'; p++)
		{
			if (*p == self->delim || *p == self->quote ||
				*p == self->escape || *p == '\n' || *p == '\r')
			{
				need_quote = true;
				break;
			}
		}
	}

	if (!need_quote)
	{
		appendStringInfoString(out, str);
		return;
	}

	appendStringInfoCharMacro(out, self->quote);
	for (p = str; *p != '\0'; p++)
	{
		if (*p == self->quote || *p == self->escape)
			appendStringInfoCharMacro(out, self->escape);
		appendStringInfoCharMacro(out, *p);
	}
	appendStringInfoCharMacro(out, self->quote);
}

/**
 * @brief Format one tuple into the output buffer.
 */
static void
CsvWriterInsert(CsvWriter *self, HeapTuple tuple)
{
	int			i;
	bool		first = true;
	StringInfo	out = csv_out(self);

	/* With a sink installed the caller decides when and how to flush. */
	if (self->sink == NULL && self->buf.len >= CSV_FLUSH_THRESHOLD)
		csv_flush(self);

	heap_deform_tuple(tuple, self->base.desc, self->values, self->nulls);

	for (i = 0; i < self->natts; i++)
	{
		char   *str;

		if (!first)
			appendStringInfoCharMacro(out, self->delim);
		first = false;

		if (self->nulls[i])
		{
			/* NULL is written as the NULL string, never quoted */
			appendBinaryStringInfo(out, self->nullstr, self->nullstr_len);
			continue;
		}

		str = OutputFunctionCall(&self->outfuncs[i], self->values[i]);
		csv_put_field(self, str, false);
		pfree(str);
	}

	appendStringInfoCharMacro(out, '\n');

	FASTLOAD_PROFILE(&prof_writer_table);
}

/**
 * @brief Flush, fsync and close the output file.
 */
static WriterResult
CsvWriterClose(CsvWriter *self, bool onError)
{
	WriterResult	ret = { 0 };

	Assert(self != NULL);

	if (self->buf.len > 0)
		csv_flush(self);

	if (self->fd >= 0)
	{
		if (!onError && pg_fsync(self->fd) != 0)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not fsync CSV output file \"%s\": %m",
							self->base.output)));
		if (close(self->fd) != 0)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not close CSV output file \"%s\": %m",
							self->base.output)));
		self->fd = -1;
	}

	if (self->buf.data)
		pfree(self->buf.data);
	if (self->outfuncs)
		pfree(self->outfuncs);
	if (self->values)
		pfree(self->values);
	if (self->nulls)
		pfree(self->nulls);

	if (self->base.output)
		pfree(self->base.output);
	self->base.output = NULL;

	if (!onError && self->base.context)
		MemoryContextDelete(self->base.context);

	return ret;
}

static bool
CsvWriterParam(CsvWriter *self, const char *keyword, char *value)
{
	if (CompareKeyword(keyword, "OUTPUT"))
	{
		if (strlen(value) >= MAXPGPATH)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("CSV output file name is too long")));

		if (!is_absolute_path(value))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("relative path not allowed for OUTPUT: %s",
							value)));

		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to export to a file")));

		ASSERT_ONCE(self->base.output == NULL);
		self->base.output = pstrdup(value);
	}
	else if (CompareKeyword(keyword, "DELIMITER"))
	{
		self->delim = ParseSingleChar(value);
	}
	else if (CompareKeyword(keyword, "QUOTE"))
	{
		self->quote = ParseSingleChar(value);
		if (self->escape == '"')
			self->escape = self->quote;
	}
	else if (CompareKeyword(keyword, "ESCAPE"))
	{
		self->escape = ParseSingleChar(value);
	}
	else if (CompareKeyword(keyword, "NULL"))
	{
		if (self->nullstr)
			pfree(self->nullstr);
		self->nullstr = pstrdup(value);
	}
	else if (CompareKeyword(keyword, "HEADER"))
	{
		self->header = ParseBoolean(value);
		self->base.header = self->header;
	}
	else
		return false;	/* unknown parameter */

	return true;
}

static void
CsvWriterDumpParams(CsvWriter *self)
{
	StringInfoData	buf;
	char		   *delim;
	char		   *quote;
	char		   *escape;
	char		   *nullstr;

	initStringInfo(&buf);
	appendStringInfoString(&buf, "WRITER = CSV\n");

	delim = QuoteSingleChar(self->delim);
	quote = QuoteSingleChar(self->quote);
	escape = QuoteSingleChar(self->escape);
	nullstr = QuoteString(self->nullstr);

	appendStringInfo(&buf, "DELIMITER = %s\n", delim);
	appendStringInfo(&buf, "QUOTE = %s\n", quote);
	appendStringInfo(&buf, "ESCAPE = %s\n", escape);
	appendStringInfo(&buf, "NULL = %s\n", nullstr);
	appendStringInfo(&buf, "HEADER = %s\n", self->header ? "YES" : "NO");
	appendStringInfo(&buf, "ENCODING = %s\n", GetDatabaseEncodingName());

	LoggerLog(INFO, buf.data, 0);

	pfree(delim);
	pfree(quote);
	pfree(escape);
	pfree(nullstr);
	pfree(buf.data);
}
