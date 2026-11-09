/*
 * pg_fastload: include/pg_profile.h
 *
 *	  Copyright (c) 2007-2026, NTT, Inc.
 */

/**
 * @file
 * @brief Macro definition for profiling
 * Usage: make USE_PROFILE=1
 */
#ifndef PROFILE_H_INCLUDED
#define PROFILE_H_INCLUDED

/* Profiling routine */
#ifdef ENABLE_FASTLOAD_PROFILE
#include "portability/instr_time.h"

extern instr_time *prof_top;

extern instr_time prof_merge;
extern instr_time prof_index;
extern instr_time prof_reindex;

extern instr_time prof_reader_source;
extern instr_time prof_reader_parser;

extern instr_time prof_writer_toast;
extern instr_time prof_writer_table;
extern instr_time prof_writer_index;

extern instr_time prof_flush;
extern instr_time prof_merge_unique;
extern instr_time prof_merge_insert;
extern instr_time prof_merge_term;

/*
 * Parallel-path stage counters.  The leader fills the first five; the
 * worker-side SCAN/FORM/IPC times are published through the DSM control
 * block (each worker is a separate process, so process-local globals do
 * not propagate) and summed by the leader before printing.
 *
 *  prof_par_setup   DSM creation, reader/worker registration, attach handshake
 *  prof_par_wait    leader blocked in shm_mq_receive() / backoff sleep
 *  prof_par_copy    private writable copy of one shared-memory tuple
 *  prof_par_write   WriterInsert() + MemoryContextReset()
 *  prof_par_read    reader process: single-pass scan + route (DSM-published)
 *  prof_par_scan    worker ring wait / input acquisition (total - form - ipc)
 *  prof_par_form    worker field split + input functions + heap_form_tuple
 *  prof_par_ipc     worker shm_mq_send()
 *
 * The whole-run GLOBAL stage (prof_parallel) is file-static in
 * pg_fastload.c and therefore intentionally not declared here.
 */
extern double prof_par_setup;
extern double prof_par_wait;
extern double prof_par_copy;
extern double prof_par_write;
extern double prof_par_read;
extern double prof_par_scan;
extern double prof_par_form;
extern double prof_par_ipc;

/**
 * @brief Record profile information
 */
#define FASTLOAD_PROFILE(name) \
	do { \
		instr_time now; \
		INSTR_TIME_SET_CURRENT(now); \
		INSTR_TIME_ACCUM_DIFF(*(name), now, *prof_top); \
		*prof_top = now; \
	} while (0);
#define FASTLOAD_PROFILE_PUSH() \
	do { \
		instr_time		_prof; \
		instr_time	   *_prof_save; \
		INSTR_TIME_SET_CURRENT(_prof); \
		_prof_save = prof_top; \
		prof_top = &_prof;

#define FASTLOAD_PROFILE_POP() \
		prof_top = _prof_save; \
	} while (0)
#else
#define FASTLOAD_PROFILE(x)		((void) 0)
#define FASTLOAD_PROFILE_PUSH()	((void) 0)
#define FASTLOAD_PROFILE_POP()	((void) 0)
#endif

#endif   /* PROFILE_H_INCLUDED */
