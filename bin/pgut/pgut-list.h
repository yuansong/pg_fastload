/*-------------------------------------------------------------------------
 *
 * pgut-list.h
 *
 * Copyright (c) 2009-2026, NTT, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGUT_LIST_H
#define PGUT_LIST_H

#if PG_VERSION_NUM >= 170000
#include "utils/palloc.h"
#endif

#if PG_VERSION_NUM >= 190000
/*
 * PostgreSQL 19 moved the Datum typedef from c.h into postgres.h, which the
 * client-side code cannot include.  The node headers below still refer to
 * Datum, so provide the very same definition here.
 */
typedef uint64_t Datum;
#endif

#include "nodes/pg_list.h"

/*
 * pgut-list.c : Extended list functions
 */
extern void list_walk(List *list, void (*walker)());
extern void list_destroy(List *list, void (*walker)());

#endif /* PGUT_LIST_H */
