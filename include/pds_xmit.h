#ifndef PDS_XMIT_H
#define PDS_XMIT_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "ussxmit.h"
#include "netdata.h"

/* In-process PDS and PDSE streaming export to Netdata stream */
int xmit_pds_stream(const ussxmit_opts_t *opts, const char *src_dsn,
                    uint16_t dsorg, uint8_t recfm, uint32_t lrecl, uint32_t blksize,
                    netdata_writer_t *writer);

#endif /* PDS_XMIT_H */
