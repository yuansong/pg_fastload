/*
 * pg_fastload: include/pg_fastload.h
 *
 *	  Copyright (c) 2007-2026, NTT, Inc.
 */
#ifndef FASTLOAD_H_INCLUDED
#define FASTLOAD_H_INCLUDED

#include "postgres.h"

#undef ENABLE_GSS
#undef USE_SSL

/*
 * One tree serves every supported major: PostgreSQL 15 through 19.
 *
 * Differences between those majors are resolved here and in the other
 * PG_VERSION_NUM branches at compile time (plus the version-pinned nbtree
 * builder, see docs/PORTING.md).  There is no per-major branch to keep in
 * sync, and therefore no second place for a fix to go missing.
 *
 * Refusing anything older is deliberate: an untested combination fails at
 * compile time instead of producing a binary that misbehaves at run time.
 */
#if PG_VERSION_NUM < 150000
#error pg_fastload requires PostgreSQL 15 or later (see docs/PORTING.md).
#endif

#include "access/tupdesc.h"

/**
 * @file
 * @brief General definition in pg_fastload.
 */

/*-------------------------------------------------------------------------
 *
 * Parser -> Writer
 * |              |
 * +--------------+
 *      Loader
 *
 * Source:
 *  - FileSource   : from local file
 *  - RemoteSource : from remote client using copy protocol
 *
 * Parser:
 *  - BinaryParser : known as FixedParser before
 *  - CSVParser    : csv file
 *  - TupleParser  : almost noop
 *
 * Writer:
 *  - BufferedWriter : to file using shared buffers
 *  - DirectLoader   : to file using local buffers
 *  - ParallelWriter : to another process
 *
 *-------------------------------------------------------------------------
 */
typedef struct Source	Source;
typedef struct Parser	Parser;
typedef struct Writer	Writer;
typedef struct Reader	Reader;

typedef enum ON_DUPLICATE
{
	ON_DUPLICATE_KEEP_NEW,
	ON_DUPLICATE_KEEP_OLD
} ON_DUPLICATE;

extern const char *ON_DUPLICATE_NAMES[2];

typedef Parser *(*ParserCreate)(void);

#define PG_FASTLOAD_COLS	8

/*
 * 64bit integer utils
 */

#ifndef INT64_MAX
#ifdef LLONG_MAX
#define INT64_MAX	LLONG_MAX
#else
#define INT64_MAX	INT64CONST(0x7FFFFFFFFFFFFFFF)
#endif
#endif

#if SIZEOF_LONG == 8
#define int64_FMT		"%ld"
#else
#define int64_FMT		"%lld"
#endif

#if !defined(__GNUC__) || (__GNUC__ == 2 && __GNUC_MINOR__ < 96)
#define __builtin_expect(x, expected_value) (x)
#endif

#ifndef likely
#define likely(x)   __builtin_expect((x),1)
#endif

#ifndef unlikely
#define unlikely(x) __builtin_expect((x),0)
#endif

#endif   /* FASTLOAD_H_INCLUDED */
