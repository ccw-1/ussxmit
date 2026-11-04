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

typedef struct {
    const ussxmit_opts_t *opts;
    netdata_info_t        info;
    char                  target_dsn[64];
    FILE                 *out_fp;
    bool                  target_allocated;
    size_t                records_restored;
    int                   error_code;
} receive_state_t;

static int receive_record_cb(bool is_control, const uint8_t *data, size_t len, void *user_data)
{
    receive_state_t *st = (receive_state_t *)user_data;
    char msg[256];

    if (is_control) {
        if (len >= 6 && memcmp(data, inmr01_eye, 6) == 0) {
            log_verbose("Processing INMR01 transmission header");
            st->info.has_inmr01 = true;
            if (len > 6) {
                netdata_parse_text_units(data + 6, len - 6, &st->info, false);
            }
        } else if (len >= 6 && memcmp(data, inmr02_eye, 6) == 0) {
            log_verbose("Processing INMR02 dataset attributes");
            st->info.has_inmr02 = true;
            if (len > 10) {
                netdata_parse_text_units(data + 10, len - 10, &st->info, true);
            }

            if (st->target_dsn[0] == '\0') {
                if (st->info.dataset_name[0] != '\0') {
                    dsn_normalize(st->target_dsn, sizeof(st->target_dsn), st->info.dataset_name);
                } else {
                    log_error("Target dataset name not specified and not present in header");
                    st->error_code = RC_ERR_SYNTAX;
                    return -1;
                }
            }

            log_info("Restoring dataset to: %s", st->target_dsn);

            if (st->opts->force) {
                dynalloc_delete_dsn(st->target_dsn);
            }

            char recfm_str[16];
            if ((st->info.recfm & RECFM_U) == RECFM_U) {
                snprintf(recfm_str, sizeof(recfm_str), "u");
            } else if (st->info.recfm & RECFM_V) {
                snprintf(recfm_str, sizeof(recfm_str), (st->info.recfm & RECFM_B) ? "v,b" : "v");
            } else {
                snprintf(recfm_str, sizeof(recfm_str), (st->info.recfm & RECFM_B) ? "f,b" : "f");
            }

            uint32_t lrecl = st->info.lrecl ? st->info.lrecl : 80;
            uint32_t blksize = st->info.blksize ? st->info.blksize : 3120;

            char dir_str[32] = "";
            if (st->info.dsorg & DSORG_PO) {
                uint32_t dir_blk = st->info.dir_blocks ? st->info.dir_blocks : 20;
                snprintf(dir_str, sizeof(dir_str), " dir(%u)", dir_blk);
            }

            char alloc_cmd[256];
            snprintf(alloc_cmd, sizeof(alloc_cmd),
                     "alloc fi(RCVOUT) da('%s') new catalog tracks space(10,50)%s recfm(%s) lrecl(%u) blksize(%u)",
                     st->target_dsn, dir_str, recfm_str, lrecl, blksize);

            log_verbose("Allocating target dataset: %s", alloc_cmd);
            int rc = dynalloc_str(alloc_cmd, msg, sizeof(msg));
            if (rc != 0) {
                log_error("Failed allocating target dataset '%s': %s (rc=%d)", st->target_dsn, msg, rc);
                st->error_code = RC_ERR_ALLOC_TGT;
                return -1;
            }
            st->target_allocated = true;

            st->out_fp = fopen("DD:RCVOUT", "wb,type=record");
            if (!st->out_fp) {
                log_error("Failed to open DD:RCVOUT for dataset '%s': %s", st->target_dsn, strerror(errno));
                dynalloc_str("free fi(RCVOUT)", msg, sizeof(msg));
                st->error_code = RC_ERR_ALLOC_TGT;
                return -1;
            }
        } else if (len >= 6 && memcmp(data, inmr06_eye, 6) == 0) {
            log_verbose("Processing INMR06 trailer (end of transmission)");
            return 1; /* Stop processing */
        }
    } else {
        /* Data Record */
        if (!st->out_fp) {
            log_error("Encountered data segment before dataset allocation");
            st->error_code = RC_ERR_STREAM;
            return -1;
        }

        if (fwrite(data, 1, len, st->out_fp) != len) {
            log_error("Failed writing record %zu to dataset", st->records_restored + 1);
            st->error_code = RC_ERR_STREAM;
            return -1;
        }
        st->records_restored++;
    }

    return 0;
}

int do_receive(const ussxmit_opts_t *opts)
{
    if (!opts) return RC_ERR_SYNTAX;

    receive_state_t state;
    memset(&state, 0, sizeof(state));
    state.opts = opts;

    if (opts->target_dsn[0] != '\0') {
        dsn_normalize(state.target_dsn, sizeof(state.target_dsn), opts->target_dsn);
    }

    uint8_t card_buf[80];
    netdata_reader_t reader;
    netdata_reader_init(&reader, receive_record_cb, &state);

    while (fread(card_buf, 1, 80, stdin) == 80) {
        int rc = netdata_reader_feed_card(&reader, card_buf);
        if (rc > 0) {
            /* Clean stop on INMR06 */
            break;
        }
        if (rc < 0) {
            if (state.error_code == 0) state.error_code = RC_ERR_STREAM;
            break;
        }
    }

    char msg[256];
    if (state.out_fp) {
        fclose(state.out_fp);
    }
    if (state.target_allocated) {
        dynalloc_str("free fi(RCVOUT)", msg, sizeof(msg));
    }

    if (state.error_code != 0) {
        return state.error_code;
    }

    log_info("Successfully restored %zu records to dataset %s", state.records_restored, state.target_dsn);
    return RC_SUCCESS;
}
