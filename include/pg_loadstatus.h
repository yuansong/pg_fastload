/*
 * pg_fastload: include/pg_loadstatus.h
 *
 *	  Copyright (c) 2007-2026, NTT, Inc.
 *	  Portions Copyright (c) 2026, yuanyuansong
 */

/**
 * @file
 * @brief Declaration of loading status proccesing module
 *
 */
#ifndef LOADSTATUS_H
#define LOADSTATUS_H

#include "storage/block.h"
#if PG_VERSION_NUM >= 160000
#include "storage/relfilelocator.h"
#else
#include "storage/relfilenode.h"
#endif

#ifndef MAXPGPATH
#define MAXPGPATH		1024
#endif

#define FASTLOAD_LSF_DIR		"pg_fastload"

/* typical sector size is 512 byte */
#define FASTLOAD_LSF_BLCKSZ		512

#if PG_VERSION_NUM >= 160000
#define FASTLOAD_LSF_PATH(buffer, ls) \
	snprintf((buffer), MAXPGPATH, \
			 FASTLOAD_LSF_DIR "/%d.%d.loadstatus", \
			 (ls)->ls.rLocator.dbOid, (ls)->ls.relid)
#else
#define FASTLOAD_LSF_PATH(buffer, ls) \
	snprintf((buffer), MAXPGPATH, \
			 FASTLOAD_LSF_DIR "/%d.%d.loadstatus", \
			 (ls)->ls.rnode.dbNode, (ls)->ls.relid)
#endif
/**
 * @brief Loading status information
 */
typedef union LoadStatus
{
	struct
	{
		Oid			relid;		/**< target relation oid */
#if PG_VERSION_NUM >= 160000
		RelFileLocator rLocator;
#else
		RelFileNode	rnode;		/**< target relation node */
#endif
		BlockNumber exist_cnt;	/**< number of blocks already existing */
		BlockNumber create_cnt;	/**< number of blocks pg_fastload creates */
	} ls;
	char	padding[FASTLOAD_LSF_BLCKSZ];
} LoadStatus;

#endif   /* LOADSTATUS_H */
