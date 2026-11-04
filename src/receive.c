#define _EXT 1
#define _XOPEN_SOURCE_EXTENDED 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

#include "ussxmit.h"
#include "netdata.h"
#include "dynalloc.h"
#include "utils.h"

static const uint8_t inmr01_eye[6] = {0xC9, 0xD5, 0xD4, 0xD9, 0xF0, 0xF1};
static const uint8_t inmr02_eye[6] = {0xC9, 0xD5, 0xD4, 0xD9, 0xF0, 0xF2};
static const uint8_t inmr06_eye[6] = {0xC9, 0xD5, 0xD4, 0xD9, 0xF0, 0xF6};

int do_receive(const ussxmit_opts_t *opts)
{
    if (!opts) return RC_ERR_SYNTAX;

    /* Stream state */
    netdata_info_t info;
    memset(&info, 0, sizeof(info));

    char target_dsn[64] = {0};
    if (opts->target_dsn[0] != '\0') {
        dsn_normalize(target_dsn, sizeof(target_dsn), opts->target_dsn);
    }

    FILE *out_fp = NULL;
    char msg[256];
    bool target_allocated = false;
    size_t records_restored = 0;

    /* Spanned logical record assembly buffer */
    uint8_t rec_assembly[65536];
    size_t  rec_assembly_len = 0;

    /* Continuous stream buffer */
    uint8_t  stream_buf[4096];
    size_t   stream_len = 0;
    size_t   stream_pos = 0;
    bool     eof_reached = false;

    while (true) {
        /* Keep stream buffer topped up */
        if (stream_pos > 0 && stream_pos < stream_len) {
            memmove(stream_buf, stream_buf + stream_pos, stream_len - stream_pos);
            stream_len -= stream_pos;
            stream_pos = 0;
        } else if (stream_pos >= stream_len) {
            stream_len = 0;
            stream_pos = 0;
        }

        if (stream_len < 256 && !eof_reached) {
            size_t n = fread(stream_buf + stream_len, 1, sizeof(stream_buf) - stream_len, stdin);
            if (n > 0) {
                stream_len += n;
            } else {
                eof_reached = true;
            }
        }

        if (stream_len - stream_pos < 2) {
            break; /* No more segment data */
        }

        uint8_t seg_len = stream_buf[stream_pos];
        uint8_t flags   = stream_buf[stream_pos + 1];

        if (seg_len < 2) {
            /* Padding or end of stream */
            break;
        }

        if (stream_pos + seg_len > stream_len) {
            if (eof_reached) {
                log_error("Truncated XMIT stream segment at offset %zu (need %u bytes, have %zu)",
                          stream_pos, seg_len, stream_len - stream_pos);
                break;
            }
            /* Need more bytes from stdin */
            continue;
        }

        const uint8_t *payload = stream_buf + stream_pos + 2;
        size_t payload_len = seg_len - 2;

        /* Process Segment */
        if (flags & 0x20) {
            /* Control Record */
            if (payload_len >= 6 && memcmp(payload, inmr01_eye, 6) == 0) {
                log_verbose("Processing INMR01 transmission header");
            } else if (payload_len >= 6 && memcmp(payload, inmr02_eye, 6) == 0) {
                log_verbose("Processing INMR02 dataset attributes");
                netdata_parse_header(stream_buf + stream_pos, seg_len, &info);

                if (target_dsn[0] == '\0') {
                    if (info.dataset_name[0] != '\0') {
                        dsn_normalize(target_dsn, sizeof(target_dsn), info.dataset_name);
                    } else {
                        log_error("Target dataset name not specified and not present in header");
                        return RC_ERR_SYNTAX;
                    }
                }

                log_info("Restoring dataset to: %s", target_dsn);

                /* If force enabled, delete existing dataset */
                if (opts->force) {
                    dynalloc_delete_dsn(target_dsn);
                }

                /* Allocate target dataset via BPXWDYN */
                char recfm_str[16];
                if ((info.recfm & RECFM_U) == RECFM_U) {
                    snprintf(recfm_str, sizeof(recfm_str), "u");
                } else if (info.recfm & RECFM_V) {
                    snprintf(recfm_str, sizeof(recfm_str), (info.recfm & RECFM_B) ? "v,b" : "v");
                } else {
                    snprintf(recfm_str, sizeof(recfm_str), (info.recfm & RECFM_B) ? "f,b" : "f");
                }

                uint32_t lrecl = info.lrecl ? info.lrecl : 80;
                uint32_t blksize = info.blksize ? info.blksize : 3120;

                char alloc_cmd[256];
                snprintf(alloc_cmd, sizeof(alloc_cmd),
                         "alloc fi(RCVOUT) da('%s') new catalog tracks space(10,50) recfm(%s) lrecl(%u) blksize(%u)",
                         target_dsn, recfm_str, lrecl, blksize);

                log_verbose("Allocating target dataset: %s", alloc_cmd);
                int rc = dynalloc_str(alloc_cmd, msg, sizeof(msg));
                if (rc != 0) {
                    log_error("Failed allocating target dataset '%s': %s (rc=%d)", target_dsn, msg, rc);
                    return RC_ERR_ALLOC_TGT;
                }
                target_allocated = true;

                /* Open target dataset */
                out_fp = fopen("DD:RCVOUT", "wb,type=record");
                if (!out_fp) {
                    log_error("Failed to open DD:RCVOUT for dataset '%s': %s", target_dsn, strerror(errno));
                    dynalloc_str("free fi(RCVOUT)", msg, sizeof(msg));
                    return RC_ERR_ALLOC_TGT;
                }
            } else if (payload_len >= 6 && memcmp(payload, inmr06_eye, 6) == 0) {
                log_verbose("Processing INMR06 trailer (end of transmission)");
                stream_pos += seg_len;
                break;
            }
        } else {
            /* Data Record */
            if (!out_fp) {
                log_error("Encountered data segment before INMR02 dataset allocation");
                return RC_ERR_STREAM;
            }

            if ((flags & 0xC0) == 0xC0) {
                /* Complete unspanned logical record */
                if (fwrite(payload, 1, payload_len, out_fp) != payload_len) {
                    log_error("Failed writing record %zu to dataset", records_restored + 1);
                    break;
                }
                records_restored++;
            } else if (flags & 0x80) {
                /* First segment of spanned record */
                rec_assembly_len = 0;
                if (payload_len < sizeof(rec_assembly)) {
                    memcpy(rec_assembly, payload, payload_len);
                    rec_assembly_len = payload_len;
                }
            } else {
                /* Middle or last segment */
                if (rec_assembly_len + payload_len < sizeof(rec_assembly)) {
                    memcpy(rec_assembly + rec_assembly_len, payload, payload_len);
                    rec_assembly_len += payload_len;
                }
                if (flags & 0x40) {
                    /* Last segment of spanned record */
                    if (fwrite(rec_assembly, 1, rec_assembly_len, out_fp) != rec_assembly_len) {
                        log_error("Failed writing spanned record %zu to dataset", records_restored + 1);
                        break;
                    }
                    records_restored++;
                    rec_assembly_len = 0;
                }
            }
        }

        stream_pos += seg_len;
    }

    if (out_fp) {
        fclose(out_fp);
    }

    if (target_allocated) {
        dynalloc_str("free fi(RCVOUT)", msg, sizeof(msg));
    }

    log_info("Successfully restored %zu records to dataset %s", records_restored, target_dsn);
    return RC_SUCCESS;
}
