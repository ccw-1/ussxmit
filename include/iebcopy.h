#ifndef IEBCOPY_H
#define IEBCOPY_H

#include <stddef.h>

/* The IEBCOPY unload dataset used between a PDSE and the XMIT stream:
 * RECFM=VS, LRECL=32756, BLKSIZE=3120 (what TSO TRANSMIT/RECEIVE use). */
#define IEBCOPY_UNLOAD_LRECL    32756
#define IEBCOPY_UNLOAD_BLKSIZE  3120

/* Allocate a new cataloged sequential unload dataset of about size_bytes
 * (0 = unknown) and DD name ddname. */
int iebcopy_alloc_unload(const char *dsn, const char *ddname, unsigned long long size_bytes);

/* Run IEBCOPY "COPY OUTDD=SYSUT2,INDD=SYSUT1" under the TSO terminal monitor
 * program (tsocmd + REXX; IEBCOPY is authorized and cannot be LINKed from a
 * USS process) from in_dsn to out_dsn, both cataloged. The IEBCOPY messages are returned
 * in log (ASCII, one line per record, newline separated). Returns the
 * IEBCOPY return code (0 and 4 are success), or a negative value when it
 * could not be run. */
int iebcopy_copy(const char *in_dsn, const char *out_dsn, char *log, size_t logsz);

#endif /* IEBCOPY_H */
