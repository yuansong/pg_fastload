/*
 * pg_fastload: include/export.h
 *
 *	  Copyright (c) 2026, yuanyuansong
 */

/**
 * @file
 * @brief Declaration of the table export module
 */
#ifndef EXPORT_H_INCLUDED
#define EXPORT_H_INCLUDED

#include "postgres.h"

/* SQL-callable: write a relation or a query result to CSV/BINARY/FIXED. */
extern Datum pg_fastload_export(PG_FUNCTION_ARGS);

#endif   /* EXPORT_H_INCLUDED */
