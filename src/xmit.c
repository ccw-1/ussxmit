#define _EXT 1
#define _XOPEN_SOURCE_EXTENDED 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "ussxmit.h"
#include "netdata.h"
#include "dynalloc.h"
#include "utils.h"

int do_xmit(const ussxmit_opts_t *opts)
{
    if (!opts || opts->source_dsn[0] == '\0') {
        log_error("No source dataset name specified for xmit action");
        return RC_ERR_SYNTAX;
    }

    char src_dsn[64];
    dsn_normalize(src_dsn, sizeof(src_dsn), opts->source_dsn);

    log_verbose("Opening source dataset: %s", src_dsn);

    /* Allocate source dataset to DD */
    char alloc_cmd[256];
    char msg[256];
    snprintf(alloc_cmd, sizeof(alloc_cmd), "alloc fi(XMITIN) da('%s') shr", src_dsn);
    int rc = dynalloc_str(alloc_cmd, msg, sizeof(msg));
    if (rc != 0) {
        log_error("Failed to dynamically allocate source dataset '%s': %s (rc=%d)", src_dsn, msg, rc);
        return RC_ERR_ALLOC_SRC;
    }

    /* Open via DD */
    FILE *fp = fopen("DD:XMITIN", "rb,type=record");
    if (!fp) {
        log_error("Failed to open DD:XMITIN for dataset '%s': %s", src_dsn, strerror(errno));
        dynalloc_str("free fi(XMITIN)", msg, sizeof(msg));
        return RC_ERR_ALLOC_SRC;
    }

    /* Retrieve dataset metadata via fldata */
    fldata_t info;
    char fname[128];
    if (fldata(fp, fname, &info) != 0) {
        log_error("fldata() failed for dataset '%s'", src_dsn);
        fclose(fp);
        dynalloc_str("free fi(XMITIN)", msg, sizeof(msg));
        return RC_ERR_INTERNAL;
    }

    uint16_t dsorg = info.__dsorgPO ? DSORG_PO : DSORG_PS;
    uint8_t recfm = 0;
    if (info.__recfmU)      recfm |= RECFM_U;
    else if (info.__recfmV) recfm |= RECFM_V;
    else if (info.__recfmF) recfm |= RECFM_F;
    if (info.__recfmBlk)    recfm |= RECFM_B;
    if (info.__recfmS)      recfm |= RECFM_S;
    if (info.__recfmASA)    recfm |= RECFM_A;
    if (info.__recfmM)      recfm |= RECFM_M;

    uint32_t lrecl = (uint32_t)info.__maxreclen;
    uint32_t blksize = (uint32_t)info.__blksize;
    if (lrecl == 0) lrecl = 80;
    if (blksize == 0) blksize = lrecl;

    log_verbose("Dataset attributes: DSORG=0x%04X, RECFM=0x%02X, LRECL=%u, BLKSIZE=%u",
                dsorg, recfm, lrecl, blksize);

    /* Initialize Netdata stream writer to STDOUT */
    netdata_writer_t writer;
    netdata_writer_init(&writer, stdout);

    /* Emit INMR01 Transmission Header */
    if (netdata_writer_emit_inmr01(&writer, "POK", get_current_hlq()) != 0) {
        log_error("Failed to emit INMR01 record");
        fclose(fp);
        dynalloc_str("free fi(XMITIN)", msg, sizeof(msg));
        return RC_ERR_STREAM;
    }

    /* Emit INMR02 Dataset Utility / Attributes */
    if (netdata_writer_emit_inmr02(&writer, src_dsn, dsorg, recfm, lrecl, blksize) != 0) {
        log_error("Failed to emit INMR02 record");
        fclose(fp);
        dynalloc_str("free fi(XMITIN)", msg, sizeof(msg));
        return RC_ERR_STREAM;
    }

    /* Read dataset records directly and stream Netdata data segments */
    uint8_t rec_buf[32768];
    size_t count = 0;
    size_t nread;

    while ((nread = fread(rec_buf, 1, sizeof(rec_buf), fp)) > 0) {
        if (netdata_writer_emit_data(&writer, rec_buf, nread) != 0) {
            log_error("Failed writing data segment for record %zu", count + 1);
            fclose(fp);
            dynalloc_str("free fi(XMITIN)", msg, sizeof(msg));
            return RC_ERR_STREAM;
        }
        count++;
    }

    /* Emit INMR06 Trailer */
    if (netdata_writer_emit_inmr06(&writer) != 0) {
        log_error("Failed to emit INMR06 record");
        fclose(fp);
        dynalloc_str("free fi(XMITIN)", msg, sizeof(msg));
        return RC_ERR_STREAM;
    }

    /* Flush and pad final 80-byte record */
    netdata_writer_flush(&writer);

    fclose(fp);
    dynalloc_str("free fi(XMITIN)", msg, sizeof(msg));

    log_verbose("Successfully streamed %zu records (%zu physical 80-byte blocks) from %s to stdout",
                count, writer.total_records_written, src_dsn);
    return RC_SUCCESS;
}
