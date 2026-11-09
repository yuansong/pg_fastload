/*
 * pg_fastload: include/common.h
 *
 *	  Copyright (c) 2010-2026, NTT, Inc.
 */

/**
 * @file
 * @brief Common definition in pg_fastload.
 *
 */
#ifndef COMMON_H_INCLUDED
#define COMMON_H_INCLUDED

#define PG_FASTLOAD_VERSION "1.0.0"


#ifndef PG_FASTLOAD_PRINTF_ATTRIBUTE
#ifdef WIN32
#define PG_FASTLOAD_PRINTF_ATTRIBUTE gnu_printf
#else
#define PG_FASTLOAD_PRINTF_ATTRIBUTE printf
#endif
#endif

#endif   /* COMMON_H_INCLUDED */
