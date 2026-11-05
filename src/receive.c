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

#define MAX_RECV_MEMBERS 4096

static const uint8_t inmr01_eye[6] = {0xC9, 0xD5, 0xD4, 0xD9, 0xF0, 0xF1};
static const uint8_t inmr02_eye[6] = {0xC9, 0xD5, 0xD4, 0xD9, 0xF0, 0xF2};
static const uint8_t inmr06_eye[6] = {0xC9, 0xD5, 0xD4, 0xD9, 0xF0, 0xF6};

typedef struct {
    char    name_ascii[9];
    uint8_t name_ebcdic[8];
    bool    is_alias;
} recv_member_t;

typedef struct {
    const ussxmit_opts_t *opts;
    netdata_info_t        info;
    char                  target_dsn[64];
    FILE                 *out_fp;
    bool                  target_allocated;
    size_t                records_restored;
    int                   error_code;

    /* PDS / PDSE specific state */
    bool                  is_pds;
    bool                  seen_copyr1;
    recv_member_t         members[MAX_RECV_MEMBERS];
    size_t                member_count;
    size_t                current_member_idx;
    FILE                 *out_mem_fp;
    bool                  mem_allocated;
    size_t                members_restored;
} receive_state_t;

static void close_current_member(receive_state_t *st)
{
    char msg[256];
    if (st->out_mem_fp) {
        fclose(st->out_mem_fp);
        st->out_mem_fp = NULL;
    }
    if (st->mem_allocated) {
        dynalloc_str("free fi(RCVMEM)", msg, sizeof(msg));
        st->mem_allocated = false;
    }
}

static int open_member_for_writing(receive_state_t *st, const char *mbr_name)
{
    close_current_member(st);

    char alloc_cmd[256];
    char msg[256];
    snprintf(alloc_cmd, sizeof(alloc_cmd), "alloc fi(RCVMEM) da('%s(%s)') shr",
             st->target_dsn, mbr_name);
    int rc = dynalloc_str(alloc_cmd, msg, sizeof(msg));
    if (rc != 0) {
        log_error("Failed allocating member '%s(%s)': %s", st->target_dsn, mbr_name, msg);
        st->error_code = RC_ERR_ALLOC_TGT;
        return -1;
    }
    st->mem_allocated = true;

    st->out_mem_fp = fopen("DD:RCVMEM", "wb,type=record");
    if (!st->out_mem_fp) {
        log_error("Failed opening DD:RCVMEM for '%s(%s)': %s",
                  st->target_dsn, mbr_name, strerror(errno));
        dynalloc_str("free fi(RCVMEM)", msg, sizeof(msg));
        st->mem_allocated = false;
        st->error_code = RC_ERR_ALLOC_TGT;
        return -1;
    }

    log_verbose("Opened member '%s(%s)' for restore", st->target_dsn, mbr_name);
    return 0;
}

static void parse_pds_directory_record(receive_state_t *st, const uint8_t *data, size_t len)
{
    if (len < 20) return;
    const uint8_t *dirblk = data + 20;
    size_t blk_avail = len - 20;
    if (blk_avail < 2) return;

    uint16_t used = (dirblk[0] << 8) | dirblk[1];
    if (used > blk_avail) used = blk_avail;
    if (used > 256) used = 256;

    size_t off = 2;
    while (off + 12 <= used && st->member_count < MAX_RECV_MEMBERS) {
        if (dirblk[off] == 0xFF) break;

        recv_member_t *m = &st->members[st->member_count];
        memset(m, 0, sizeof(*m));
        memcpy(m->name_ebcdic, dirblk + off, 8);
        uint8_t c = dirblk[off + 11];
        m->is_alias = (c & 0x80) != 0;

        memcpy(m->name_ascii, m->name_ebcdic, 8);
        m->name_ascii[8] = '\0';
        e2a_str(m->name_ascii);
        for (int i = 7; i >= 0; i--) {
            if (m->name_ascii[i] == ' ' || m->name_ascii[i] == '\0') {
                m->name_ascii[i] = '\0';
            } else {
                break;
            }
        }

        uint8_t user_halfwords = c & 0x1F;
        off += 12 + (user_halfwords * 2);

        if (!m->is_alias) {
            log_verbose("Queued member '%s' for restore", m->name_ascii);
            st->member_count++;
        }
    }
}

static int process_pds_ckd_data(receive_state_t *st, const uint8_t *data, size_t len)
{
    /* Check for COPYR1 header (56 bytes, 00 CA 6D 0F ...) */
    if (len == 56 && data[1] == 0xCA && data[2] == 0x6D && data[3] == 0x0F) {
        log_verbose("Received IEBCOPY COPYR1 header");
        st->seen_copyr1 = true;
        return 0;
    }

    /* Check for Directory header / block records (276 or 288 bytes) */
    if (len == 276 || len == 288) {
        log_verbose("Processing IEBCOPY directory record (len=%zu)", len);
        parse_pds_directory_record(st, data, len);
        return 0;
    }

    /* Process CKD member data blocks */
    size_t off = 0;
    while (off + 12 <= len) {
        uint32_t block_len = ((uint32_t)data[off + 9] << 16) |
                             ((uint32_t)data[off + 10] << 8) |
                             (uint32_t)data[off + 11];
        off += 12;

        if (block_len > 0) {
            if (off + block_len > len) {
                block_len = len - off;
            }

            if (!st->out_mem_fp) {
                if (st->current_member_idx < st->member_count) {
                    const char *mname = st->members[st->current_member_idx].name_ascii;
                    if (open_member_for_writing(st, mname) != 0) {
                        return -1;
                    }
                } else {
                    log_warn("Received CKD block without active member in directory");
                }
            }

            if (st->out_mem_fp) {
                if (fwrite(data + off, 1, block_len, st->out_mem_fp) != block_len) {
                    log_error("Failed writing member block");
                    st->error_code = RC_ERR_STREAM;
                    return -1;
                }
                st->records_restored++;
            }
            off += block_len;
        } else {
            /* EOF for current member */
            log_verbose("Completed member '%s'",
                        (st->current_member_idx < st->member_count) ?
                        st->members[st->current_member_idx].name_ascii : "unknown");
            close_current_member(st);
            st->members_restored++;
            st->current_member_idx++;
        }
    }

    return 0;
}

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

            if (st->target_allocated) {
                return 0; /* Already allocated on first INMR02 */
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
            st->is_pds = (st->info.dsorg & DSORG_PO) != 0;
            if (st->is_pds) {
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

            if (st->is_pds) {
                /* For PDS, free the container DD since members will be allocated individually */
                dynalloc_str("free fi(RCVOUT)", msg, sizeof(msg));
                st->target_allocated = false;
            } else {
                /* For sequential dataset, open container for record writing */
                st->out_fp = fopen("DD:RCVOUT", "wb,type=record");
                if (!st->out_fp) {
                    log_error("Failed to open DD:RCVOUT for dataset '%s': %s", st->target_dsn, strerror(errno));
                    dynalloc_str("free fi(RCVOUT)", msg, sizeof(msg));
                    st->error_code = RC_ERR_ALLOC_TGT;
                    return -1;
                }
            }
        } else if (len >= 6 && memcmp(data, inmr06_eye, 6) == 0) {
            log_verbose("Processing INMR06 trailer (end of transmission)");
            return 1; /* Stop processing */
        }
    } else {
        /* Data Record */
        if (st->is_pds) {
            if (process_pds_ckd_data(st, data, len) != 0) {
                return -1;
            }
        } else {
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
    if (state.is_pds) {
        close_current_member(&state);
    } else {
        if (state.out_fp) {
            fclose(state.out_fp);
        }
        if (state.target_allocated) {
            dynalloc_str("free fi(RCVOUT)", msg, sizeof(msg));
        }
    }

    if (state.error_code != 0) {
        return state.error_code;
    }

    if (state.is_pds) {
        log_info("Successfully restored %zu members (%zu blocks) to PDS %s",
                 state.members_restored, state.records_restored, state.target_dsn);
    } else {
        log_info("Successfully restored %zu records to dataset %s",
                 state.records_restored, state.target_dsn);
    }
    return RC_SUCCESS;
}
