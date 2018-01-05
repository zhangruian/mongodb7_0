/*-
 * Copyright (c) 2014-2018 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_epoch_raw --
 *	Return the time since the Epoch as reported by a system call.
 */
void
__wt_epoch_raw(WT_SESSION_IMPL *session, struct timespec *tsp)
{
	WT_DECL_RET;

	/*
	 * This function doesn't return an error, but panics on failure (which
	 * should never happen, it's done this way to simplify error handling
	 * in the caller). However, some compilers complain about using garbage
	 * values. Initializing the values avoids the complaint.
	 */
	tsp->tv_sec = 0;
	tsp->tv_nsec = 0;

#if defined(HAVE_CLOCK_GETTIME)
	WT_SYSCALL_RETRY(clock_gettime(CLOCK_REALTIME, tsp), ret);
	if (ret == 0)
		return;
	WT_PANIC_MSG(session, ret, "clock_gettime");
#elif defined(HAVE_GETTIMEOFDAY)
	{
	struct timeval v;

	WT_SYSCALL_RETRY(gettimeofday(&v, NULL), ret);
	if (ret == 0) {
		tsp->tv_sec = v.tv_sec;
		tsp->tv_nsec = v.tv_usec * WT_THOUSAND;
		return;
	}
	WT_PANIC_MSG(session, ret, "gettimeofday");
	}
#else
	NO TIME-OF-DAY IMPLEMENTATION: see src/os_posix/os_time.c
#endif
}
