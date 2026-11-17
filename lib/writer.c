/*
 * pg_fastload: lib/writer.c
 *
 *	  Copyright (c) 2011-2026, NTT, Inc.
 *	  Portions Copyright (c) 2026, yuanyuansong
 */

/**
 * @file
 * @brief Implementation of writer module
 */
#include "pg_fastload.h"

#include "utils/builtins.h"
#include "utils/lsyscache.h"

#include "pg_strutil.h"
#include "logger.h"
#include "reader.h"
#include "writer.h"

const char *ON_DUPLICATE_NAMES[] =
{
	"NEW",
	"OLD"
};

/**
 * @brief Create Writer
 */
Writer *
WriterCreate(char *writer, bool multi_process)
{
	const char *keys[] =
	{
		"DIRECT",
		"BUFFERED",
		"BINARY"
	};
	const CreateWriter values[] =
	{
		CreateDirectWriter,
		CreateBufferedWriter,
		CreateBinaryWriter
	};

	Writer *self;

	/* default of writer is DIRECT */
	if (writer == NULL)
		writer = "DIRECT";

	/* alias for backward compatibility. */
	if (pg_strcasecmp(writer, "PARALLEL") == 0)
	{
		multi_process = true;
		writer = "DIRECT";
	}

	self = values[choice("WRITER", writer, keys, lengthof(keys))](NULL);

	if (multi_process)
		self = CreateParallelWriter(self);

	self->multi_process = multi_process;

	return self;
}

void
WriterInit(Writer *self)
{
	self->init(self);
}

/**
 * @brief Parse a line in control file.
 */
bool
WriterParam(Writer *self, const char *keyword, char *value)
{
	if (CompareKeyword(keyword, "VERBOSE"))
	{
		self->verbose = ParseBoolean(value);
	}
	else if (!self->param(self, keyword, value))
		return false;

	return true;
}

void
WriterDumpParams(Writer *self)
{
	char		   *str;
	StringInfoData	buf;

	initStringInfo(&buf);

	str = QuoteString(self->output);
	appendStringInfo(&buf, "OUTPUT = %s\n", str);
	pfree(str);

	appendStringInfo(&buf, "MULTI_PROCESS = %s\n", self->multi_process ? "YES" : "NO");

	appendStringInfo(&buf, "VERBOSE = %s\n", self->verbose ? "YES" : "NO");

	LoggerLog(INFO, buf.data, 0);
	pfree(buf.data);

	self->dumpParams(self);
}

WriterResult
WriterClose(Writer *self, bool onError)
{
	if (self->dup_badfile != NULL)
		pfree(self->dup_badfile);

	self->dup_badfile = NULL;

	return self->close(self, onError);
}

/*
 * Qualified name of a relation, or NULL when there is none to name.
 *
 * Not every load has a target relation: WRITER = BINARY writes a file, and
 * the progress record is published before the loader knows whether the OID
 * it was handed still resolves.  quote_qualified_identifier() cannot take a
 * NULL identifier (quote_identifier() reads ident[0]), so the empty cases
 * have to be filtered out here rather than at each call site.
 */
char *
get_relation_name(Oid relid)
{
	char	   *relname;
	char	   *nspname;

	if (!OidIsValid(relid))
		return NULL;

	relname = get_rel_name(relid);
	if (relname == NULL)
		return NULL;			/* relation dropped meanwhile */

	nspname = get_namespace_name(get_rel_namespace(relid));

	return quote_qualified_identifier(nspname, relname);
}

