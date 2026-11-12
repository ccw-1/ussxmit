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
#include "iebcopy.h"
#include "utils.h"

#define MAX_RECV_MEMBERS 4096

/* Space allocation when restoring (see INMR02 handling) */
#define RECV_MAX_SPACE_UNITS       16777215u /* largest primary/secondary BPXWDYN accepts */
#define RECV_SECONDARY_DIVISOR     4u        /* secondary extent = 1/4 of the primary */
#define RECV_DEFAULT_PRIMARY_CYL   1u        /* used when the sender gave no size */
#define RECV_DEFAULT_SECONDARY_CYL 500u
#define RECV_DEFAULT_DIR_BLOCKS    20u       /* used when a PDS stream has no INMDIR */

static const uint8_t inmr01_eye[6] = {0xC9, 0xD5, 0xD4, 0xD9, 0xF0, 0xF1};
static const uint8_t inmr02_eye[6] = {0xC9, 0xD5, 0xD4, 0xD9, 0xF0, 0xF2};
static const uint8_t inmr06_eye[6] = {0xC9, 0xD5, 0xD4, 0xD9, 0xF0, 0xF6};

typedef struct {
    char    name_ascii[9];
    uint8_t name_ebcdic[8];
    uint8_t ttr[3];            /* directory TTR: relative track and record of the first block */
    bool    is_alias;
    bool    done;
} recv_member_t;

#define RECV_MAX_EXTENTS 16

typedef struct {
    uint32_t cyl, head, ntracks;
} recv_extent_t;

enum { PDS_PHASE_COPYR1 = 0, PDS_PHASE_COPYR2, PDS_PHASE_DIR, PDS_PHASE_DATA, PDS_PHASE_DONE };

typedef struct {
    const ussxmit_opts_t *opts;
    netdata_info_t        info;
    char                  target_dsn[64];
    FILE                 *out_fp;
    bool                  target_allocated;
    bool                  inmr02_seen;      /* TSO streams carry several INMR02; only the first describes the target */
    size_t                records_restored;
    int                   error_code;

    /* PDS / PDSE specific state */
    bool                  is_pds;
    bool                  seen_copyr1;
    int                   pds_phase;        /* PDS_PHASE_* : COPYR1, COPYR2, directory, member data */
    uint8_t               hdr_buf[12];      /* member data stream decoder state */
    size_t                blk_kl;           /* key length of the unit being collected (directory blocks) */
    size_t                hdr_have;
    uint8_t               blk_buf[65536];
    size_t                blk_need;
    size_t                blk_have;
    recv_member_t         members[MAX_RECV_MEMBERS];
    size_t                member_count;
    int                   cur_member;       /* member the data stream is in, -1 between members */
    uint32_t              heads;            /* tracks per cylinder of the unloaded volume */
    recv_extent_t         extents[RECV_MAX_EXTENTS];
    size_t                extent_count;
    FILE                 *out_mem_fp;
    bool                  mem_allocated;
    size_t                members_restored;

    /* PDSE (and other COPYR1 flagged) streams are restored through IEBCOPY:
     * the unload records go to a temporary sequential data set first. */
    bool                  po_alloc_pending; /* target PO data set is allocated when COPYR1 shows its kind */
    char                  po_alloc_cmd[320];
    bool                  use_iebcopy;
    char                  unload_dsn[64];
    FILE                 *unload_fp;
    bool                  unload_allocated;

    /* PDSE data libraries are written member by member unless the directory
     * has aliases or user data (ISPF statistics), which only IEBCOPY can
     * restore: the records up to the end of the directory are kept so they
     * can be replayed into the IEBCOPY unload data set. */
    bool                  pdse_native;
    bool                  dir_has_extras;
    bool                  switch_pending;
    bool                  blk_skip;         /* the unit being collected is discarded */
    uint8_t              *replay;
    size_t                replay_len, replay_cap;
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

static void parse_pds_directory_block(receive_state_t *st, const uint8_t *dirblk, size_t blk_avail)
{
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
        memcpy(m->ttr, dirblk + off + 8, 3);
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
        if (m->is_alias || user_halfwords) st->dir_has_extras = true;
        off += 12 + (user_halfwords * 2);

        if (!m->is_alias) {
            log_verbose("Queued member '%s' for restore", m->name_ascii);
            st->member_count++;
        }
    }
}

/* Write one physical block of a member as the logical records it holds.
 * Unloaded members carry whole DASD blocks: fixed blocks are cut into LRECL
 * records, variable blocks are decoded from their BDW/RDW, undefined blocks
 * are one record. */
static int write_member_block(receive_state_t *st, const uint8_t *blk, size_t len)
{
    if (!st->out_mem_fp) {
        if (st->cur_member >= 0) {
            const char *mname = st->members[st->cur_member].name_ascii;
            if (open_member_for_writing(st, mname) != 0) {
                return -1;
            }
        } else {
            log_error("Received CKD block without active member in directory");
            st->error_code = RC_ERR_STREAM;
            return -1;
        }
    }

    if ((st->info.recfm & RECFM_U) == RECFM_U) {
        if (fwrite(blk, 1, len, st->out_mem_fp) != len) goto fail;
        st->records_restored++;
    } else if (st->info.recfm & RECFM_V) {
        size_t bdw = len >= 4 ? (((size_t)blk[0] << 8) | blk[1]) : 0;
        if (bdw > len) bdw = len;
        size_t p = 4;
        while (p + 4 <= bdw) {
            size_t rdw = ((size_t)blk[p] << 8) | blk[p + 1];
            if (rdw < 4 || p + rdw > bdw) break;
            if (fwrite(blk + p + 4, 1, rdw - 4, st->out_mem_fp) != rdw - 4) goto fail;
            st->records_restored++;
            p += rdw;
        }
    } else {
        size_t reclen = st->info.lrecl > 0 ? st->info.lrecl : len;
        for (size_t done = 0; done < len; done += reclen) {
            size_t n = (len - done < reclen) ? len - done : reclen;
            if (fwrite(blk + done, 1, n, st->out_mem_fp) != n) goto fail;
            st->records_restored++;
        }
    }
    return 0;

fail:
    log_error("Failed writing member block");
    st->error_code = RC_ERR_STREAM;
    return -1;
}

/* Find the member whose data starts with the block header h: its CCHHR is
 * turned into a relative track through the extents of COPYR2 and compared with
 * the directory TTRs; without a match the next member in directory order is used. */
static int select_member(receive_state_t *st, const uint8_t *h)
{
    uint64_t pos = ((uint64_t)((h[4] << 8) | h[5])) * st->heads + ((h[6] << 8) | h[7]);
    uint32_t base = 0;
    bool mapped = false;
    uint32_t tt = 0;
    for (size_t e = 0; e < st->extent_count; e++) {
        uint64_t start = (uint64_t)st->extents[e].cyl * st->heads + st->extents[e].head;
        if (pos >= start && pos - start < st->extents[e].ntracks) {
            tt = base + (uint32_t)(pos - start);
            mapped = true;
            break;
        }
        base += st->extents[e].ntracks;
    }
    if (mapped) {
        for (size_t i = 0; i < st->member_count; i++) {
            const recv_member_t *m = &st->members[i];
            if (!m->done && m->ttr[0] == (uint8_t)(tt >> 8) && m->ttr[1] == (uint8_t)tt && m->ttr[2] == h[8]) {
                return (int)i;
            }
        }
    }
    for (size_t i = 0; i < st->member_count; i++) {
        if (!st->members[i].done) return (int)i;
    }
    return -1;
}

/* PDSE unload: the header holds the member's id in the TTR position (CCHH = 0, TT; R) */
static int select_member_id(const receive_state_t *st, const uint8_t *h)
{
    for (size_t i = 0; i < st->member_count; i++) {
        const recv_member_t *m = &st->members[i];
        if (!m->done && m->ttr[0] == h[6] && m->ttr[1] == h[7] && m->ttr[2] == h[8]) return (int)i;
    }
    return -1;
}

/* Header of a unit of the PDSE member stream */
static int process_pdse_header(receive_state_t *st)
{
    const uint8_t *h = st->hdr_buf;
    uint8_t flag = h[0];
    size_t len = ((size_t)h[9] << 16) | ((size_t)h[10] << 8) | h[11];

    switch (flag) {
    case 0x00:      /* data block */
    case 0x80:      /* end of the data of a member */
        if (st->cur_member < 0) st->cur_member = select_member_id(st, h);
        if (st->cur_member < 0) {
            log_error("Received a member unit without a matching directory entry");
            st->error_code = RC_ERR_STREAM;
            return -1;
        }
        if (flag == 0x80) {
            const char *mname = st->members[st->cur_member].name_ascii;
            if (!st->out_mem_fp && open_member_for_writing(st, mname) != 0) return -1;   /* empty member */
            log_verbose("Completed member '%s'", mname);
            close_current_member(st);
            st->members[st->cur_member].done = true;
            st->members_restored++;
            st->cur_member = -1;
            return 0;
        }
        if (len == 0) return 0;
        break;
    case 0x04:      /* member attributes */
    case 0x02:      /* dataset attributes */
        st->blk_skip = true;
        break;
    case 0x84:
        return 0;
    default:        /* 0xC2: end of the unload; anything else is padding */
        st->pds_phase = PDS_PHASE_DONE;
        return 0;
    }
    if (len > sizeof(st->blk_buf)) {
        log_error("Member block of %zu bytes exceeds buffer", len);
        st->error_code = RC_ERR_STREAM;
        return -1;
    }
    st->blk_need = len;
    st->blk_have = 0;
    return 0;
}

/* Consume the member data stream: [12-byte header][block] units, which a
 * logical record boundary may cut anywhere. A zero-length header ends a member. */
static int process_member_bytes(receive_state_t *st, const uint8_t *p, size_t len)
{
    while (len > 0) {
        if (st->pds_phase == PDS_PHASE_DONE) return 0;
        if (st->blk_need == 0) {
            size_t n = 12 - st->hdr_have;
            if (n > len) n = len;
            memcpy(st->hdr_buf + st->hdr_have, p, n);
            st->hdr_have += n;
            p += n;
            len -= n;
            if (st->hdr_have < 12) break;
            st->hdr_have = 0;
            if (st->pds_phase == PDS_PHASE_DIR) {
                /* directory unit: count field with KL (byte 9) and DL (bytes 10-11); all zero ends it */
                size_t kl = st->hdr_buf[9];
                size_t dl = ((size_t)st->hdr_buf[10] << 8) | st->hdr_buf[11];
                if (kl == 0 && dl == 0) {
                    st->pds_phase = PDS_PHASE_DATA;
                    if (st->pdse_native && st->dir_has_extras) {
                        st->switch_pending = true;      /* the caller replays the records into IEBCOPY */
                        return 0;
                    }
                } else {
                    st->blk_kl = kl;
                    st->blk_need = kl + dl;
                    st->blk_have = 0;
                }
                continue;
            }

            if (st->pdse_native && st->pds_phase == PDS_PHASE_DATA) {
                if (process_pdse_header(st) != 0) return -1;
                continue;
            }

            if (st->cur_member < 0) {
                st->cur_member = select_member(st, st->hdr_buf);
            }

            size_t block_len = ((size_t)st->hdr_buf[9] << 16) | ((size_t)st->hdr_buf[10] << 8) | st->hdr_buf[11];
            if (block_len == 0) {
                log_verbose("Completed member '%s'",
                            st->cur_member >= 0 ? st->members[st->cur_member].name_ascii : "unknown");
                close_current_member(st);
                st->members_restored++;
                if (st->cur_member >= 0) st->members[st->cur_member].done = true;
                st->cur_member = -1;
            } else {
                if (block_len > sizeof(st->blk_buf)) {
                    log_error("Member block of %zu bytes exceeds buffer", block_len);
                    st->error_code = RC_ERR_STREAM;
                    return -1;
                }
                st->blk_need = block_len;
                st->blk_have = 0;
            }
        } else {
            size_t n = st->blk_need - st->blk_have;
            if (n > len) n = len;
            memcpy(st->blk_buf + st->blk_have, p, n);
            st->blk_have += n;
            p += n;
            len -= n;
            if (st->blk_have == st->blk_need) {
                size_t blen = st->blk_need;
                st->blk_need = 0;
                if (st->pds_phase == PDS_PHASE_DIR) {
                    log_verbose("Processing IEBCOPY directory block");
                    parse_pds_directory_block(st, st->blk_buf + st->blk_kl, blen - st->blk_kl);
                    continue;
                }
                if (st->blk_skip) {
                    st->blk_skip = false;
                    continue;
                }
                if (write_member_block(st, st->blk_buf, blen) != 0) return -1;
            }
        }
    }
    return 0;
}

static int process_pds_record(receive_state_t *st, const uint8_t *data, size_t len)
{
    switch (st->pds_phase) {
    case PDS_PHASE_COPYR1:
        /* COPYR1 header (56 bytes, 00 CA 6D 0F ...) */
        if (len >= 4 && data[1] == 0xCA && data[2] == 0x6D && data[3] == 0x0F) {
            log_verbose("Received IEBCOPY COPYR1 header");
            st->seen_copyr1 = true;
            st->heads = (len >= 28) ? (((uint32_t)data[26] << 8) | data[27]) : 0;
            if (st->heads == 0) st->heads = 15;
            st->pds_phase = PDS_PHASE_COPYR2;
            return 0;
        }
        log_warn("Unexpected record before IEBCOPY COPYR1 header (len=%zu)", len);
        return 0;

    case PDS_PHASE_COPYR2:
        /* extent list: 16-byte entries from offset 20 (start CCHH at +2, end CCHH, track count) */
        st->extent_count = 0;
        for (size_t off = 20; off + 12 <= len && st->extent_count < RECV_MAX_EXTENTS; off += 16) {
            uint32_t nt = ((uint32_t)data[off + 10] << 8) | data[off + 11];
            if (nt == 0) break;
            recv_extent_t *x = &st->extents[st->extent_count++];
            x->cyl = ((uint32_t)data[off + 2] << 8) | data[off + 3];
            x->head = ((uint32_t)data[off + 4] << 8) | data[off + 5];
            x->ntracks = nt;
        }
        st->pds_phase = PDS_PHASE_DIR;
        return 0;

    default: /* directory units and member data are one byte stream */
        return process_member_bytes(st, data, len);
    }
}

/* Create the unload data set the records of an IEBCOPY restore are collected in */
static int start_iebcopy_unload(receive_state_t *st)
{
    dynalloc_temp_dsn(st->unload_dsn, sizeof(st->unload_dsn), "UNLD");
    if (iebcopy_alloc_unload(st->unload_dsn, "UNLOAD", st->info.size_bytes) != 0) {
        st->error_code = RC_ERR_ALLOC_TGT;
        return -1;
    }
    st->unload_allocated = true;
    st->unload_fp = fopen("DD:UNLOAD", "wb,type=record");
    if (!st->unload_fp) {
        log_error("Failed to open the unload data set '%s': %s", st->unload_dsn, strerror(errno));
        st->error_code = RC_ERR_ALLOC_TGT;
        return -1;
    }
    st->use_iebcopy = true;
    return 0;
}

static int write_unload_record(receive_state_t *st, const uint8_t *data, size_t len)
{
    if (fwrite(data, 1, len, st->unload_fp) != len) {
        log_error("Failed writing unload record %zu: %s", st->records_restored + 1, strerror(errno));
        st->error_code = RC_ERR_STREAM;
        return -1;
    }
    st->records_restored++;
    return 0;
}

/* Allocate the PO target once COPYR1 tells whether the unload comes from a PDSE
 * (first byte of COPYR1: X'01' data PDSE, X'10'/X'11' program objects; a PDS has
 * X'00'). A data PDSE is written member by member; program libraries (and any
 * PDSE when USSXMIT_IEBCOPY is set) go through IEBCOPY. */
static int allocate_po_target(receive_state_t *st, const uint8_t *copyr1, size_t len)
{
    char msg[256];
    char alloc_cmd[400];
    bool pdse = len >= 4 && copyr1[1] == 0xCA && copyr1[2] == 0x6D && copyr1[3] == 0x0F &&
                (copyr1[0] & 0x11) != 0;
    bool native = pdse && copyr1[0] == 0x01 && (st->info.recfm & RECFM_U) != RECFM_U &&
                  getenv("USSXMIT_IEBCOPY") == NULL;

    snprintf(alloc_cmd, sizeof(alloc_cmd), "%s%s", st->po_alloc_cmd, pdse ? " dsntype(library)" : "");
    log_verbose("Allocating target dataset: %s", alloc_cmd);
    int rc = dynalloc_str(alloc_cmd, msg, sizeof(msg));
    if (rc != 0) {
        log_error("Failed allocating target dataset '%s': %s (rc=%d)", st->target_dsn, msg, rc);
        st->error_code = RC_ERR_ALLOC_TGT;
        return -1;
    }
    /* Members are allocated individually (PDS, native PDSE) or by IEBCOPY */
    dynalloc_str("free fi(RCVOUT)", msg, sizeof(msg));
    st->po_alloc_pending = false;

    if (native) {
        st->pdse_native = true;
        log_verbose("Data PDSE stream: restoring member by member");
    } else if (pdse) {
        if (start_iebcopy_unload(st) != 0) return -1;
        log_verbose("PDSE stream: restoring through IEBCOPY via unload data set %s", st->unload_dsn);
    }
    return 0;
}

/* Keep the records up to the end of the directory of a native PDSE restore */
static int replay_append(receive_state_t *st, const uint8_t *data, size_t len)
{
    size_t need = st->replay_len + 4 + len;
    if (need > st->replay_cap) {
        size_t cap = st->replay_cap ? st->replay_cap : 65536;
        while (cap < need) cap *= 2;
        uint8_t *n = realloc(st->replay, cap);
        if (!n) {
            log_error("Out of memory buffering the PDSE directory");
            st->error_code = RC_ERR_STREAM;
            return -1;
        }
        st->replay = n;
        st->replay_cap = cap;
    }
    uint8_t *q = st->replay + st->replay_len;
    q[0] = (uint8_t)(len >> 24); q[1] = (uint8_t)(len >> 16); q[2] = (uint8_t)(len >> 8); q[3] = (uint8_t)len;
    memcpy(q + 4, data, len);
    st->replay_len = need;
    return 0;
}

static void replay_free(receive_state_t *st)
{
    free(st->replay);
    st->replay = NULL;
    st->replay_len = st->replay_cap = 0;
}

/* The directory has aliases or user data that only IEBCOPY restores: move to the
 * IEBCOPY path and feed it the records seen so far. */
static int switch_to_iebcopy(receive_state_t *st)
{
    log_verbose("PDSE directory has aliases or user data: restoring through IEBCOPY");
    if (start_iebcopy_unload(st) != 0) return -1;
    st->pdse_native = false;
    st->switch_pending = false;
    for (size_t off = 0; off + 4 <= st->replay_len; ) {
        const uint8_t *q = st->replay + off;
        size_t n = ((size_t)q[0] << 24) | ((size_t)q[1] << 16) | ((size_t)q[2] << 8) | q[3];
        if (write_unload_record(st, q + 4, n) != 0) return -1;
        off += 4 + n;
    }
    replay_free(st);
    return 0;
}

static int process_pds_ckd_data(receive_state_t *st, const uint8_t *data, size_t len)
{
    if (st->po_alloc_pending && allocate_po_target(st, data, len) != 0) return -1;

    if (st->use_iebcopy) return write_unload_record(st, data, len);

    if (st->pdse_native && st->pds_phase < PDS_PHASE_DATA && replay_append(st, data, len) != 0) return -1;

    int rc = process_pds_record(st, data, len);
    if (rc != 0) return rc;
    if (st->switch_pending) return switch_to_iebcopy(st);
    if (st->pds_phase >= PDS_PHASE_DATA) replay_free(st);
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
            if (st->inmr02_seen) {
                log_verbose("Ignoring additional INMR02 (describes the intermediate unload dataset)");
                return 0;
            }
            st->inmr02_seen = true;
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
                uint32_t dir_blk = st->info.dir_blocks ? st->info.dir_blocks : RECV_DEFAULT_DIR_BLOCKS;
                snprintf(dir_str, sizeof(dir_str), " dir(%u)", dir_blk);
            }

            /* Size the target like TSO RECEIVE does: from INMSIZE (approximate
             * bytes) when the sender supplied it, in units of blocks so the
             * system accounts for track capacity itself. Further data goes
             * into secondary extents, and unused space is released on close. */
            char space_str[96];
            if (st->info.size_bytes > 0) {
                uint64_t blocks = (st->info.size_bytes + blksize - 1) / blksize;
                uint64_t secondary = blocks / RECV_SECONDARY_DIVISOR;
                if (blocks > RECV_MAX_SPACE_UNITS) blocks = RECV_MAX_SPACE_UNITS;
                if (secondary < 1) secondary = 1;
                if (secondary > RECV_MAX_SPACE_UNITS) secondary = RECV_MAX_SPACE_UNITS;
                snprintf(space_str, sizeof(space_str), "block(%u) space(%llu,%llu) release",
                         blksize, (unsigned long long)blocks, (unsigned long long)secondary);
            } else {
                /* No size in the stream (e.g. XMIT370): small primary, large
                 * secondary extents. */
                snprintf(space_str, sizeof(space_str), "cyl space(%u,%u) release",
                         RECV_DEFAULT_PRIMARY_CYL, RECV_DEFAULT_SECONDARY_CYL);
            }

            char alloc_cmd[320];
            char lrecl_str[24] = "";
            if ((st->info.recfm & RECFM_U) != RECFM_U) {
                snprintf(lrecl_str, sizeof(lrecl_str), " lrecl(%u)", lrecl);   /* none for RECFM U */
            }
            snprintf(alloc_cmd, sizeof(alloc_cmd),
                     "alloc fi(RCVOUT) da('%s') new catalog %s%s recfm(%s)%s blksize(%u)",
                     st->target_dsn, space_str, dir_str, recfm_str, lrecl_str, blksize);

            if (st->is_pds) {
                /* Defer: COPYR1 (first data record) tells PDS from PDSE */
                snprintf(st->po_alloc_cmd, sizeof(st->po_alloc_cmd), "%s", alloc_cmd);
                st->po_alloc_pending = true;
                st->target_allocated = false;
                return 0;
            }

            log_verbose("Allocating target dataset: %s", alloc_cmd);
            int rc = dynalloc_str(alloc_cmd, msg, sizeof(msg));
            if (rc != 0) {
                log_error("Failed allocating target dataset '%s': %s (rc=%d)", st->target_dsn, msg, rc);
                st->error_code = RC_ERR_ALLOC_TGT;
                return -1;
            }
            st->target_allocated = true;

            /* Sequential dataset: open the container for record writing */
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

    static receive_state_t state;
    memset(&state, 0, sizeof(state));
    state.opts = opts;
    state.cur_member = -1;

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
    if (state.is_pds && state.po_alloc_pending && state.error_code == 0) {
        allocate_po_target(&state, NULL, 0);    /* empty stream: create the empty PDS */
    }
    replay_free(&state);
    if (state.is_pds) {
        close_current_member(&state);
        if (state.pdse_native && state.error_code == 0 && state.members_restored != state.member_count) {
            log_error("PDSE stream ended after %zu of %zu members", state.members_restored, state.member_count);
            state.error_code = RC_ERR_STREAM;
        }
    } else {
        if (state.out_fp) {
            fclose(state.out_fp);
        }
        if (state.target_allocated) {
            dynalloc_str("free fi(RCVOUT)", msg, sizeof(msg));
        }
    }

    if (state.unload_fp) {
        fclose(state.unload_fp);
        state.unload_fp = NULL;
    }
    if (state.unload_allocated) {
        dynalloc_str("free fi(UNLOAD)", msg, sizeof(msg));
    }
    if (state.use_iebcopy && state.error_code == 0) {
        static char iebcopy_log[16384];
        int irc = iebcopy_copy(state.unload_dsn, state.target_dsn, iebcopy_log, sizeof(iebcopy_log));
        if (irc < 0 || irc > 4) {
            log_error("IEBCOPY failed with return code %d restoring %s", irc, state.target_dsn);
            if (iebcopy_log[0]) fprintf(stderr, "%s", iebcopy_log);
            state.error_code = RC_ERR_TSO;
        } else if (iebcopy_log[0]) {
            log_verbose("IEBCOPY messages:\n%s", iebcopy_log);
        }
    }
    if (state.unload_allocated) {
        dynalloc_delete_dsn(state.unload_dsn);
    }

    if (state.error_code != 0) {
        return state.error_code;
    }

    if (state.use_iebcopy) {
        log_info("Successfully restored PDSE %s through IEBCOPY (%zu unload records)",
                 state.target_dsn, state.records_restored);
    } else if (state.is_pds) {
        log_info("Successfully restored %zu members (%zu blocks) to %s %s",
                 state.members_restored, state.records_restored,
                 state.pdse_native ? "PDSE" : "PDS", state.target_dsn);
    } else {
        log_info("Successfully restored %zu records to dataset %s",
                 state.records_restored, state.target_dsn);
    }
    return RC_SUCCESS;
}
