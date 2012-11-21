/* combined imklog driver for BSD and Linux
 *
 * This contains OS-specific functionality to read the BSD
 * or Linux kernel log. For a general overview, see head comment in
 * imklog.c. This started out as the BSD-specific drivers, but it
 * turned out that on modern Linux the implementation details
 * are very small, and so we use a single driver for both OS's with
 * a little help of conditional compilation.
 *
 * Copyright 2008-2012 Adiscon GmbH
 *
 * This file is part of rsyslog.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *       http://www.apache.org/licenses/LICENSE-2.0
 *       -or-
 *       see COPYING.ASL20 in the source distribution
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#ifdef	OS_LINUX
#	include <sys/klog.h>
#endif

#include "rsyslog.h"
#include "srUtils.h"
#include "debug.h"
#include "imklog.h"

/* globals */
static int	fklog = -1;	/* kernel log fd */

#ifndef _PATH_KLOG
#	ifdef OS_LINUX
#	  define _PATH_KLOG "/proc/kmsg"
#	else
#	  define _PATH_KLOG "/dev/klog"
#	endif
#endif


#ifdef OS_LINUX
/* submit a message to imklog Syslog() API. In this function, we check if 
 * a kernel timestamp is present and, if so, extract and strip it.
 * Note: this is an extra processing step. We should revisit the whole
 * idea in v6 and remove all that old stuff that we do not longer need
 * (like symbol resolution). <-- TODO 
 * Note that this is heavily Linux specific and thus is not compiled or
 * used for BSD.
 * Special thanks to Lennart Poettering for suggesting on how to convert
 * the kernel timestamp to a realtime timestamp. This method depends on 
 * the fact the the kernel timestamp is written using the monotonic clock.
 * Shall that change (very unlikely), this code must be changed as well. Note
 * that due to the way we generate the delta, we are unable to write the
 * absolutely correct timestamp (system call overhead of the clock calls
 * prevents us from doing so). However, the difference is very minor.
 * rgerhards, 2011-06-24
 */
static void
submitSyslog(int pri, uchar *buf)
{
	long secs;
	long nsecs;
	long secOffs;
	long nsecOffs;
	unsigned i;
	unsigned bufsize;
	struct timespec monotonic, realtime;
	struct timeval tv;
	struct timeval *tp = NULL;

	if(buf[3] != '[')
		goto done;
	DBGPRINTF("imklog: kernel timestamp detected, extracting it\n");

	/* we now try to parse the timestamp. iff it parses, we assume
	 * it is a timestamp. Otherwise we know for sure it is no ts ;)
	 */
	i = 4; /* space or first digit after '[' */
	while(buf[i] && isspace(buf[i]))
		++i; /* skip space */
	secs = 0;
	while(buf[i] && isdigit(buf[i])) {
		secs = secs * 10 + buf[i] - '0';
		++i;
	}
	if(buf[i] != '.') {
		DBGPRINTF("no dot --> no kernel timestamp\n");
		goto done; /* no TS! */
	}
	
	++i; /* skip dot */
	nsecs = 0;
	while(buf[i] && isdigit(buf[i])) {
		nsecs = nsecs * 10 + buf[i] - '0';
		++i;
	}
	if(buf[i] != ']') {
		DBGPRINTF("no trailing ']' --> no kernel timestamp\n");
		goto done; /* no TS! */
	}
	++i; /* skip ']' */

	/* we have a timestamp */
	DBGPRINTF("kernel timestamp is %ld %ld\n", secs, nsecs);
	if(!bKeepKernelStamp) {
		bufsize= strlen((char*)buf);
		memmove(buf+3, buf+i, bufsize - i + 1);
	}

	clock_gettime(CLOCK_MONOTONIC, &monotonic);
	clock_gettime(CLOCK_REALTIME, &realtime);
	secOffs = realtime.tv_sec - monotonic.tv_sec;
	nsecOffs = realtime.tv_nsec - monotonic.tv_nsec;
	if(nsecOffs < 0) {
		secOffs--;
		nsecOffs += 1000000000l;
	}
	
	nsecs +=nsecOffs;
	if(nsecs > 999999999l) {
		secs++;
		nsecs -= 1000000000l;
	}
	secs += secOffs;
	tv.tv_sec = secs;
	tv.tv_usec = nsecs / 1000;
	tp = &tv;

done:
	Syslog(pri, buf, tp);
}
#else	/* now comes the BSD "code" (just a shim) */
static void
submitSyslog(int pri, uchar *buf)
{
	Syslog(pri, buf, NULL);
}
#endif	/* #ifdef LINUX */


static uchar *GetPath(void)
{
	return pszPath ? pszPath : (uchar*) _PATH_KLOG;
}

/* open the kernel log - will be called inside the willRun() imklog
 * entry point. -- rgerhards, 2008-04-09
 */
rsRetVal
klogWillRun(void)
{
	char errmsg[2048];
	int r;
	DEFiRet;

	fklog = open((char*)GetPath(), O_RDONLY, 0);
	if (fklog < 0) {
		imklogLogIntMsg(RS_RET_ERR_OPEN_KLOG, "imklog: cannot open kernel log(%s): %s.",
			GetPath(), rs_strerror_r(errno, errmsg, sizeof(errmsg)));
		ABORT_FINALIZE(RS_RET_ERR_OPEN_KLOG);
	}

#	ifdef OS_LINUX
	/* Set level of kernel console messaging.. */
	if(console_log_level != -1) {
		r = klogctl(8, NULL, console_log_level);
		if(r != 0) {
			imklogLogIntMsg(LOG_WARNING, "imklog: cannot set console log level: %s",
				rs_strerror_r(errno, errmsg, sizeof(errmsg)));
			/* make sure we do not try to re-set! */
			console_log_level = -1;
		}
	}
#	endif	/* #ifdef OS_LINUX */

finalize_it:
	RETiRet;
}


/* Read kernel log while data are available, split into lines.
 */
static void
readklog(void)
{
	char *p, *q;
	int len, i;
	int iMaxLine;
	uchar bufRcv[128*1024+1];
	char errmsg[2048];
	uchar *pRcv = NULL; /* receive buffer */

	iMaxLine = klog_getMaxLine();

	/* we optimize performance: if iMaxLine is below our fixed size buffer (which
	 * usually is sufficiently large), we use this buffer. if it is higher, heap memory
	 * is used. We could use alloca() to achive a similar aspect, but there are so
	 * many issues with alloca() that I do not want to take that route.
	 * rgerhards, 2008-09-02
	 */
	if((size_t) iMaxLine < sizeof(bufRcv) - 1) {
		pRcv = bufRcv;
	} else {
		if((pRcv = (uchar*) MALLOC(sizeof(uchar) * (iMaxLine + 1))) == NULL)
			iMaxLine = sizeof(bufRcv) - 1; /* better this than noting */
	}

	len = 0;
	for (;;) {
		dbgprintf("imklog(BSD/Linux) waiting for kernel log line\n");
		i = read(fklog, pRcv + len, iMaxLine - len);
		if (i > 0) {
			pRcv[i + len] = '\0';
		} else {
			if (i < 0 && errno != EINTR && errno != EAGAIN) {
				imklogLogIntMsg(LOG_ERR,
				       "imklog: error reading kernel log - shutting down: %s",
					rs_strerror_r(errno, errmsg, sizeof(errmsg)));
				fklog = -1;
			}
			break;
		}

		for (p = (char*)pRcv; (q = strchr(p, '\n')) != NULL; p = q + 1) {
			*q = '\0';
			submitSyslog(LOG_INFO, (uchar*) p);
		}
		len = strlen(p);
		if (len >= iMaxLine - 1) {
			submitSyslog(LOG_INFO, (uchar*)p);
			len = 0;
		}
		if(len > 0)
			memmove(pRcv, p, len + 1);
	}
	if (len > 0)
		submitSyslog(LOG_INFO, pRcv);

	if(pRcv != NULL && (size_t) iMaxLine >= sizeof(bufRcv) - 1)
		free(pRcv);
}


/* to be called in the module's AfterRun entry point
 * rgerhards, 2008-04-09
 */
rsRetVal klogAfterRun(void)
{
        DEFiRet;
	if(fklog != -1)
		close(fklog);
#	ifdef OS_LINUX
	/* Turn on logging of messages to console, but only if a log level was speficied */
	if(console_log_level != -1)
		klogctl(7, NULL, 0);
#	endif
        RETiRet;
}



/* to be called in the module's WillRun entry point, this is the main
 * "message pull" mechanism.
 * rgerhards, 2008-04-09
 */
rsRetVal klogLogKMsg(void)
{
        DEFiRet;
	readklog();
	RETiRet;
}


/* provide the (system-specific) default facility for internal messages
 * rgerhards, 2008-04-14
 */
int
klogFacilIntMsg(void)
{
	return LOG_SYSLOG;
}
