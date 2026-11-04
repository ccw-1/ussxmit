#define _EXT 1
#define _XOPEN_SOURCE_EXTENDED 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "dataset_io.h"
#include "utils.h"

int dataset_to_stream(const char *dsn, FILE *out_fp, size_t *records_written)
{
    if (!dsn || !out_fp) return -1;
    if (records_written) *records_written = 0;

    char mvs_path[128];
    char normalized[64];
    dsn_normalize(normalized, sizeof(normalized), dsn);
    snprintf(mvs_path, sizeof(mvs_path), "//'%s'", normalized);

    log_verbose("Opening dataset for reading: %s", mvs_path);
    FILE *fp = fopen(mvs_path, "rb,type=record");
    if (!fp) {
        log_error("Cannot open dataset %s for read: %s", mvs_path, strerror(errno));
        return -1;
    }

    char record[XMIT_RECORD_LEN];
    size_t count = 0;
    size_t nread;

    while ((nread = fread(record, 1, XMIT_RECORD_LEN, fp)) > 0) {
        /* If record read was less than 80 bytes, pad with spaces or flush */
        if (nread < XMIT_RECORD_LEN) {
            memset(record + nread, 0x40, XMIT_RECORD_LEN - nread);
        }
        if (fwrite(record, 1, XMIT_RECORD_LEN, out_fp) != XMIT_RECORD_LEN) {
            log_error("Failed writing record %zu to stdout: %s", count + 1, strerror(errno));
            fclose(fp);
            return -1;
        }
        count++;
    }

    fclose(fp);
    fflush(out_fp);

    if (records_written) *records_written = count;
    log_verbose("Streamed %zu records from %s to stdout", count, normalized);
    return 0;
}

int stream_to_dataset(FILE *in_fp, const char *dsn, size_t *records_read, char *header_buf, size_t header_buf_sz)
{
    if (!in_fp || !dsn) return -1;
    if (records_read) *records_read = 0;

    char mvs_path[128];
    char normalized[64];
    dsn_normalize(normalized, sizeof(normalized), dsn);
    snprintf(mvs_path, sizeof(mvs_path), "//'%s'", normalized);

    log_verbose("Opening dataset for writing: %s", mvs_path);
    FILE *fp = fopen(mvs_path, "wb,type=record");
    if (!fp) {
        log_error("Cannot open dataset %s for write: %s", mvs_path, strerror(errno));
        return -1;
    }

    char record[XMIT_RECORD_LEN];
    size_t count = 0;
    size_t header_bytes_copied = 0;

    while (fread(record, 1, XMIT_RECORD_LEN, in_fp) == XMIT_RECORD_LEN) {
        if (header_buf && header_bytes_copied + XMIT_RECORD_LEN <= header_buf_sz) {
            memcpy(header_buf + header_bytes_copied, record, XMIT_RECORD_LEN);
            header_bytes_copied += XMIT_RECORD_LEN;
        }

        if (fwrite(record, 1, XMIT_RECORD_LEN, fp) != XMIT_RECORD_LEN) {
            log_error("Failed writing record %zu to dataset %s: %s", count + 1, mvs_path, strerror(errno));
            fclose(fp);
            return -1;
        }
        count++;
    }

    fclose(fp);

    if (records_read) *records_read = count;
    log_verbose("Wrote %zu records from stdin to %s", count, normalized);
    return 0;
}
