#ifndef PDS_XMIT_H
#define PDS_XMIT_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "ussxmit.h"
#include "netdata.h"

/* Returned by xmit_pds_stream (pdse) before anything is written: use IEBCOPY */
#define XMIT_FALLBACK  (-2)

/* In-process PDS streaming export to Netdata stream. With pdse set the source
 * is a PDSE data library (RECFM F, V), unloaded in IEBCOPY's PDSE layout. */
int xmit_pds_stream(const ussxmit_opts_t *opts, const char *src_dsn,
                    uint16_t dsorg, uint8_t recfm, uint32_t lrecl, uint32_t blksize,
                    bool pdse, netdata_writer_t *writer);

/* PDSE (data or program library): unloaded by IEBCOPY through TSO */
int xmit_pdse_stream(const ussxmit_opts_t *opts, const char *src_dsn, netdata_writer_t *writer);

#endif /* PDS_XMIT_H */
