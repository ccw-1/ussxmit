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
#include "pds_xmit.h"
#include "dynalloc.h"
#include "iebcopy.h"
#include "pdse_unload.h"
#include "utils.h"

#define MAX_MEMBERS 4096
#define MAX_DIR_BLOCKS 1024

/* Virtual geometry of the unloaded dataset: a 3390 volume. */
#define UNLOAD_HEADS        15
#define UNLOAD_TRACK_BYTES  58786
#define UNLOAD_START_CYL    0x0010
#define UNLOAD_MAX_RECORD   32756      /* LRECL of the intermediate dataset */
#define UNLOAD_BLKSIZE      3120       /* BLKSIZE of the intermediate dataset */
#define TRACK_CELLS         1729       /* 3390 track capacity in 34-byte cells */

/* Cells a record without key and with data length dl occupies on a 3390 track */
static uint32_t record_cells(size_t dl)
{
    return 10 + (uint32_t)((dl + 6 + 33) / 34);
}

/* CCHH of the relative track tt in the virtual extent */
static void tt_to_cchh(uint32_t tt, uint8_t out[4])
{
    uint32_t cyl = UNLOAD_START_CYL + tt / UNLOAD_HEADS;
    uint32_t head = tt % UNLOAD_HEADS;
    out[0] = (cyl >> 8) & 0xFF; out[1] = cyl & 0xFF;
    out[2] = (head >> 8) & 0xFF; out[3] = head & 0xFF;
}

/* 12-byte block header: 4 zero bytes, CCHH, R, 3-byte block length */
static void block_header(uint8_t *h, uint32_t tt, uint8_t r, size_t len)
{
    memset(h, 0, 4);
    tt_to_cchh(tt, h + 4);
    h[8] = r;
    h[9] = (len >> 16) & 0xFF; h[10] = (len >> 8) & 0xFF; h[11] = len & 0xFF;
}

/* Send one header+block unit; units longer than the intermediate dataset's
 * LRECL continue in the next record. */
static int emit_unit(netdata_writer_t *w, const uint8_t *buf, size_t len)
{
    while (len > 0) {
        size_t n = len > UNLOAD_MAX_RECORD ? UNLOAD_MAX_RECORD : len;
        if (netdata_writer_emit_data(w, buf, n) != 0) return -1;
        buf += n;
        len -= n;
    }
    return 0;
}

typedef struct {
    char        name_ascii[9];
    uint8_t     name_ebcdic[8];
    uint8_t     ttr[3];
    uint8_t     c_byte;
    uint8_t     userdata[64];
    size_t      userdata_len;
    bool        is_alias;
} pds_member_entry_t;

/* Members are unloaded in physical order (directory TTR), not name order */
static int member_ttr_cmp(const void *a, const void *b)
{
    return memcmp(((const pds_member_entry_t *)a)->ttr, ((const pds_member_entry_t *)b)->ttr, 3);
}

/* 12-byte unit header of a PDSE unload: flag, 3 zero bytes, CCHH (CC=0, HH=TT of
 * the member's directory TTR), R, 3-byte length */
static void pdse_header(uint8_t *h, uint8_t flag, const uint8_t ttr[3], size_t len)
{
    memset(h, 0, 12);
    h[0] = flag;
    h[6] = ttr[0]; h[7] = ttr[1];
    h[8] = ttr[2];
    h[9] = (len >> 16) & 0xFF; h[10] = (len >> 8) & 0xFF; h[11] = len & 0xFF;
}

/* Member data of a PDSE data library: per non-alias member in TTR order the
 * data blocks, an end marker, the (constant) attribute record and its end
 * marker; the dataset attribute record and the end of the unload follow. */
static int emit_pdse_members(netdata_writer_t *w, const char *src_dsn,
                             const pds_member_entry_t *members, size_t count,
                             size_t *total_blocks)
{
    char cmd[256];
    char msg[256];
    uint8_t *buf = malloc(12 + 65536);
    uint8_t last_ttr[3] = {0, 0, 0};
    int rc = 0;
    if (!buf) return -1;

    for (size_t m = 0; m < count && rc == 0; m++) {
        const pds_member_entry_t *mbr = &members[m];
        if (mbr->is_alias) continue;

        snprintf(cmd, sizeof(cmd), "alloc fi(MBRDD) da('%s(%s)') shr", src_dsn, mbr->name_ascii);
        if (dynalloc_str(cmd, msg, sizeof(msg)) != 0) {
            log_error("Failed to allocate member '%s': %s", mbr->name_ascii, msg);
            rc = -1;
            break;
        }
        FILE *fmem = fopen("DD:MBRDD", "rb,type=blocked");
        if (!fmem) {
            log_error("Failed to open member '%s': %s", mbr->name_ascii, strerror(errno));
            dynalloc_str("free fi(MBRDD)", msg, sizeof(msg));
            rc = -1;
            break;
        }

        size_t n;
        size_t nblocks = 0;
        while ((n = fread(buf + 12, 1, 65536, fmem)) > 0) {
            pdse_header(buf, PDSE_FLAG_DATA, mbr->ttr, n);
            if (emit_unit(w, buf, 12 + n) != 0) { rc = -1; break; }
            nblocks++;
        }
        fclose(fmem);
        dynalloc_str("free fi(MBRDD)", msg, sizeof(msg));
        if (rc != 0) break;

        pdse_header(buf, PDSE_FLAG_END_DATA, mbr->ttr, 0);
        if (emit_unit(w, buf, 12) != 0) { rc = -1; break; }
        pdse_header(buf, PDSE_FLAG_ATTR, mbr->ttr, sizeof(pdse_member_attr));
        memcpy(buf + 12, pdse_member_attr, sizeof(pdse_member_attr));
        if (emit_unit(w, buf, 12 + sizeof(pdse_member_attr)) != 0) { rc = -1; break; }
        pdse_header(buf, PDSE_FLAG_END_ATTR, mbr->ttr, 0);
        if (emit_unit(w, buf, 12) != 0) { rc = -1; break; }

        memcpy(last_ttr, mbr->ttr, 3);
        *total_blocks += nblocks;
        log_verbose("Streamed member '%s' (%zu blocks)", mbr->name_ascii, nblocks);
    }

    if (rc == 0) {
        pdse_header(buf, PDSE_FLAG_DSATTR, last_ttr, sizeof(pdse_dataset_attr));
        memcpy(buf + 12, pdse_dataset_attr, sizeof(pdse_dataset_attr));
        if (emit_unit(w, buf, 12 + sizeof(pdse_dataset_attr)) != 0) rc = -1;
    }
    if (rc == 0) {
        pdse_header(buf, PDSE_FLAG_END_DSATTR, last_ttr, 0);
        if (emit_unit(w, buf, 12) != 0) rc = -1;
    }
    free(buf);
    return rc;
}

int xmit_pds_stream(const ussxmit_opts_t *opts, const char *src_dsn,
                    uint16_t dsorg, uint8_t recfm, uint32_t lrecl, uint32_t blksize,
                    bool pdse, netdata_writer_t *writer)
{
    char msg[256];
    char alloc_cmd[256];

    log_verbose("Starting PDS/PDSE in-process unload for: %s", src_dsn);

    /* Step 1: Allocate container to read directory blocks */
    snprintf(alloc_cmd, sizeof(alloc_cmd), "alloc fi(PDSDIR) da('%s') shr", src_dsn);
    int rc = dynalloc_str(alloc_cmd, msg, sizeof(msg));
    if (rc != 0) {
        log_error("Failed to allocate directory for '%s': %s (rc=%d)", src_dsn, msg, rc);
        return RC_ERR_ALLOC_SRC;
    }

    FILE *fdir = fopen("DD:PDSDIR", "rb,type=record");
    if (!fdir) {
        log_error("Failed to open DD:PDSDIR for '%s': %s", src_dsn, strerror(errno));
        dynalloc_str("free fi(PDSDIR)", msg, sizeof(msg));
        return RC_ERR_ALLOC_SRC;
    }

    /* Buffer directory blocks */
    uint8_t (*dir_blocks)[256] = malloc(MAX_DIR_BLOCKS * 256);
    if (!dir_blocks) {
        log_error("Out of memory allocating directory block buffer");
        fclose(fdir);
        dynalloc_str("free fi(PDSDIR)", msg, sizeof(msg));
        return RC_ERR_INTERNAL;
    }

    size_t dir_block_count = 0;
    size_t dir_used_count = 0;   /* blocks up to the one holding the end-of-directory marker */
    size_t n;
    while (dir_block_count < MAX_DIR_BLOCKS &&
           (n = fread(dir_blocks[dir_block_count], 1, 256, fdir)) > 0) {
        dir_block_count++;
        /* Check if last block ends with high key 0xFF... */
        uint8_t *blk = dir_blocks[dir_block_count - 1];
        if (n >= 2) {
            uint16_t used = (blk[0] << 8) | blk[1];
            if (dir_used_count == 0) {
                /* walk the entries: the marker is an entry whose name starts with 0xFF */
                uint16_t lim = used > n ? (uint16_t)n : used;
                size_t eoff = 2;
                while (eoff + 12 <= lim) {
                    if (blk[eoff] == 0xFF) { dir_used_count = dir_block_count; break; }
                    eoff += 12 + (blk[eoff + 11] & 0x1F) * 2;
                }
            }
            if (used >= 10 && memcmp(blk + 2, "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8) == 0) {
                break;
            }
        }
    }

    fclose(fdir);
    dynalloc_str("free fi(PDSDIR)", msg, sizeof(msg));

    log_verbose("Read %zu directory blocks from %s", dir_block_count, src_dsn);

    /* A PDSE directory read through the access method differs from the one in
     * IEBCOPY's unload: the member id (TTR) in the unload is 2 * TTR + 1, and the
     * used count of the block with the end marker stops after the 8-byte marker. */
    if (pdse) {
        for (size_t b = 0; b < dir_block_count; b++) {
            uint8_t *blk = dir_blocks[b];
            uint16_t used = (blk[0] << 8) | blk[1];
            if (used > 256) used = 256;
            memset(blk + used, 0, 256 - used);      /* bytes past the used count are stale */
            size_t off = 2;
            while (off + 12 <= used) {
                if (blk[off] == 0xFF) {
                    blk[0] = ((off + 8) >> 8) & 0xFF; blk[1] = (off + 8) & 0xFF;
                    memset(blk + off + 8, 0, 256 - (off + 8));
                    blk[off + 10] = 0x01;           /* the marker's TTR 0, mapped like the members' */
                    break;
                }
                uint32_t id = ((uint32_t)blk[off + 8] << 16) | ((uint32_t)blk[off + 9] << 8) | blk[off + 10];
                id = id * 2 + 1;
                blk[off + 8] = (id >> 16) & 0xFF; blk[off + 9] = (id >> 8) & 0xFF; blk[off + 10] = id & 0xFF;
                off += 12 + (blk[off + 11] & 0x1F) * 2;
            }
        }
    }

    /* Step 2: Parse members from directory blocks */
    pds_member_entry_t *members = malloc(MAX_MEMBERS * sizeof(pds_member_entry_t));
    if (!members) {
        log_error("Out of memory allocating member table");
        free(dir_blocks);
        return RC_ERR_INTERNAL;
    }

    size_t member_count = 0;
    for (size_t b = 0; b < dir_block_count; b++) {
        uint8_t *blk = dir_blocks[b];
        uint16_t used = (blk[0] << 8) | blk[1];
        if (used > 256) used = 256;

        size_t off = 2;
        while (off + 12 <= used && member_count < MAX_MEMBERS) {
            if (blk[off] == 0xFF) {
                break; /* High values end marker */
            }

            pds_member_entry_t *m = &members[member_count];
            memset(m, 0, sizeof(*m));

            memcpy(m->name_ebcdic, blk + off, 8);
            memcpy(m->ttr, blk + off + 8, 3);
            m->c_byte = blk[off + 11];
            m->is_alias = (m->c_byte & 0x80) != 0;

            /* Convert EBCDIC member name to ASCII */
            memcpy(m->name_ascii, m->name_ebcdic, 8);
            m->name_ascii[8] = '\0';
            e2a_str(m->name_ascii);
            /* Trim trailing whitespace */
            for (int i = 7; i >= 0; i--) {
                if (m->name_ascii[i] == ' ' || m->name_ascii[i] == '\0') {
                    m->name_ascii[i] = '\0';
                } else {
                    break;
                }
            }

            uint8_t user_halfwords = m->c_byte & 0x1F;
            size_t ulen = user_halfwords * 2;
            if (ulen > 0 && off + 12 + ulen <= used) {
                if (ulen > sizeof(m->userdata)) ulen = sizeof(m->userdata);
                memcpy(m->userdata, blk + off + 12, ulen);
                m->userdata_len = ulen;
            }

            off += 12 + (user_halfwords * 2);
            member_count++;
        }
    }

    qsort(members, member_count, sizeof(members[0]), member_ttr_cmp);

    log_info("Discovered %zu members in %s", member_count, src_dsn);


    /* Probe first member to extract exact dataset RECFM, LRECL, and BLKSIZE */
    if (member_count > 0) {
        char probe_cmd[256];
        char probe_msg[256];
        snprintf(probe_cmd, sizeof(probe_cmd), "alloc fi(PROBEDD) da('%s(%s)') shr",
                 src_dsn, members[0].name_ascii);
        if (dynalloc_str(probe_cmd, probe_msg, sizeof(probe_msg)) == 0) {
            FILE *fprobe = fopen("DD:PROBEDD", "rb,type=record");
            if (fprobe) {
                fldata_t pinfo;
                char pfname[128];
                if (fldata(fprobe, pfname, &pinfo) == 0) {
                    recfm = 0;
                    if (pinfo.__recfmU)      recfm |= RECFM_U;
                    else if (pinfo.__recfmV) recfm |= RECFM_V;
                    else if (pinfo.__recfmF) recfm |= RECFM_F;
                    if (pinfo.__recfmBlk)    recfm |= RECFM_B;
                    if (pinfo.__recfmS)      recfm |= RECFM_S;
                    if (pinfo.__recfmASA)    recfm |= RECFM_A;
                    if (pinfo.__recfmM)      recfm |= RECFM_M;

                    lrecl = (uint32_t)pinfo.__maxreclen;
                    blksize = (uint32_t)pinfo.__blksize;
                    if (lrecl == 0 && (recfm & RECFM_F)) lrecl = 80;
                    /* TRANSMIT sends the DCB LRECL, which includes the 4-byte RDW of variable records */
                    if ((recfm & RECFM_V) && (recfm & RECFM_U) != RECFM_U) lrecl += 4;
                    if (blksize == 0) blksize = (lrecl > 0) ? lrecl : 32760;
                    log_verbose("Refined PDS member attributes: RECFM=0x%02X, LRECL=%u, BLKSIZE=%u",
                                recfm, lrecl, blksize);
                }
                fclose(fprobe);
            }
            dynalloc_str("free fi(PROBEDD)", probe_msg, sizeof(probe_msg));
        }
    }

    /* The native PDSE unload covers data libraries (RECFM F or V, known from the
     * member: the directory reports RECFM U) that fit the buffers; program
     * libraries and anything else are left to IEBCOPY. Nothing has been
     * written to the stream yet. */
    if (pdse && ((recfm & RECFM_U) == RECFM_U || member_count == 0 ||
                 member_count >= MAX_MEMBERS || dir_block_count >= MAX_DIR_BLOCKS)) {
        free(members);
        free(dir_blocks);
        return XMIT_FALLBACK;
    }

    /* Approximate size in bytes (INMSIZE), as TRANSMIT reports for a PDS: the
     * directory plus every member, each rounded up to whole blocks because a
     * block never holds data of two members. The C runtime knows the record
     * count of a member, so a seek to the end is cheap. */
    uint64_t blk = blksize ? blksize : 1;
    uint64_t size_bytes = ((uint64_t)dir_block_count * 256 + blk - 1) / blk * blk;
    for (size_t m = 0; m < member_count; m++) {
        if (members[m].is_alias) continue;
        snprintf(alloc_cmd, sizeof(alloc_cmd), "alloc fi(MBRDD) da('%s(%s)') shr",
                 src_dsn, members[m].name_ascii);
        if (dynalloc_str(alloc_cmd, msg, sizeof(msg)) != 0) continue;
        FILE *fsz = fopen("DD:MBRDD", "rb,type=record");
        if (fsz) {
            if (fseek(fsz, 0, SEEK_END) == 0) {
                long nrecs = ftell(fsz);
                if (nrecs > 0) {
                    size_bytes += ((uint64_t)nrecs * lrecl + blk - 1) / blk * blk;
                }
            }
            fclose(fsz);
        }
        dynalloc_str("free fi(MBRDD)", msg, sizeof(msg));
    }
    log_verbose("Estimated dataset size: %llu bytes (%zu directory blocks)",
                (unsigned long long)size_bytes, dir_block_count);

    /* Step 3: Emit Netdata Transmission Control Records */
    /* INMR01 Transmission Header */
    if (netdata_writer_emit_inmr01(writer, "POK", get_current_hlq()) != 0) {
        log_error("Failed to emit INMR01 record");
        free(members);
        free(dir_blocks);
        return RC_ERR_STREAM;
    }

    /* INMR02 #1 describes the PDS (IEBCOPY), INMR02 #2 the intermediate unload
     * dataset (INMCOPY, VBS 32756/3120) and INMR03 its records, as TRANSMIT does. */
    writer->inmtype = pdse ? 0x80 : 0x00;
    if (netdata_writer_emit_inmr02_ex(writer, "IEBCOPY", src_dsn, dsorg, recfm, 0x00, lrecl, blksize,
                                      size_bytes, (uint32_t)(dir_block_count > 0 ? dir_block_count : 1)) != 0 ||
        netdata_writer_emit_inmr02_ex(writer, "INMCOPY", NULL, DSORG_PS, 0x48, 0x02, 32756, 3120,
                                      size_bytes, 0) != 0 ||
        netdata_writer_emit_inmr03(writer, DSORG_PS, 0x01, 80, size_bytes) != 0) {
        log_error("Failed to emit INMR02/INMR03 records");
        free(members);
        free(dir_blocks);
        return RC_ERR_STREAM;
    }

    /* Step 4: Emit IEBCOPY unloaded dataset header, COPYR2 and directory records.
     *
     * Real IEBCOPY reload locates a member through the directory TTR, so the
     * unload must describe a consistent (virtual) set of extents: a 3390-style
     * geometry (15 heads) starting at UNLOAD_START_CYL, and every block header
     * carries the CCHHR that results from the member's directory TTR. */
    uint32_t max_tt = 0;
    for (size_t m = 0; m < member_count; m++) {
        uint32_t tt = ((uint32_t)members[m].ttr[0] << 8) | members[m].ttr[1];
        if (tt > max_tt) max_tt = tt;
    }
    /* Members longer than a track continue on following tracks; reserve generously */
    uint64_t ntracks = (uint64_t)max_tt + 1 + size_bytes / 8192 + 2;
    ntracks = (ntracks + UNLOAD_HEADS - 1) / UNLOAD_HEADS * UNLOAD_HEADS;   /* whole cylinders */
    if (ntracks > 0xFFFF) ntracks = 0xFFFF;

    /* DS1LSTAR (last used TTR) is only known after streaming, but COPYR1 goes
     * first: use the highest member start from the directory. */
    uint32_t lstar_tt = max_tt;
    uint8_t  lstar_r  = 0;
    for (size_t m = 0; m < member_count; m++) {
        uint32_t tt = ((uint32_t)members[m].ttr[0] << 8) | members[m].ttr[1];
        if (tt == max_tt && members[m].ttr[2] > lstar_r) lstar_r = members[m].ttr[2];
    }

    /* 4a. COPYR1 Header (56 bytes) */
    uint8_t copyr1[56];
    memset(copyr1, 0, sizeof(copyr1));
    copyr1[1] = 0xCA; copyr1[2] = 0x6D; copyr1[3] = 0x0F;
    copyr1[4] = (dsorg >> 8) & 0xFF; copyr1[5] = dsorg & 0xFF;
    copyr1[6] = (blksize >> 8) & 0xFF; copyr1[7] = blksize & 0xFF;
    copyr1[8] = (lrecl >> 8) & 0xFF; copyr1[9] = lrecl & 0xFF;
    copyr1[10] = recfm;
    copyr1[11] = 0x00;                                  /* key length */
    copyr1[12] = 0x00; copyr1[13] = 0x80;
    copyr1[14] = 0x0C; copyr1[15] = 0x30;               /* block size of the unload dataset (3120) */
    copyr1[16] = 0x30; copyr1[17] = 0x30; copyr1[18] = 0x20; copyr1[19] = 0x0F;  /* UCB type: 3390 */
    copyr1[22] = 0x7F; copyr1[23] = 0xF8;
    copyr1[24] = 0x7F; copyr1[25] = 0xF8;
    copyr1[26] = 0x00; copyr1[27] = UNLOAD_HEADS;
    copyr1[28] = (UNLOAD_TRACK_BYTES >> 8) & 0xFF; copyr1[29] = UNLOAD_TRACK_BYTES & 0xFF;
    copyr1[32] = 0x22; copyr1[33] = 0x52;
    copyr1[37] = 0x02; copyr1[38] = 0x00; copyr1[39] = 0x7E;
    copyr1[40] = 0x01; copyr1[41] = 0x3B;
    copyr1[42] = 0x80;                                  /* DS1DSIND: last volume */
    if (blksize <= 0x1FFF) { copyr1[43] = (blksize >> 8) & 0xFF; copyr1[44] = blksize & 0xFF; }
    copyr1[45] = 0xC0;                                  /* DS1SCALO: cylinders */
    {
        uint32_t ncyl = (uint32_t)(ntracks / UNLOAD_HEADS);
        copyr1[46] = (ncyl >> 16) & 0xFF; copyr1[47] = (ncyl >> 8) & 0xFF; copyr1[48] = ncyl & 0xFF;
    }
    copyr1[49] = (lstar_tt >> 8) & 0xFF; copyr1[50] = lstar_tt & 0xFF; copyr1[51] = lstar_r;

    if (pdse) {
        memcpy(copyr1, pdse_copyr1, sizeof(copyr1));
        copyr1[4] = (dsorg >> 8) & 0xFF; copyr1[5] = dsorg & 0xFF;
        copyr1[6] = (blksize >> 8) & 0xFF; copyr1[7] = blksize & 0xFF;
        uint32_t c1_lrecl = (recfm & RECFM_U) == RECFM_U ? 0 : lrecl;
        copyr1[8] = (c1_lrecl >> 8) & 0xFF; copyr1[9] = c1_lrecl & 0xFF;
        copyr1[10] = recfm;
    }
    if (netdata_writer_emit_data(writer, copyr1, sizeof(copyr1)) != 0) {
        log_error("Failed to emit COPYR1 header record");
        free(members);
        free(dir_blocks);
        return RC_ERR_STREAM;
    }

    /* 4b. COPYR2: DEB image with the (single, virtual) extent (276 bytes) */
    uint8_t copyr2[276];
    memset(copyr2, 0, sizeof(copyr2));
    static const uint8_t deb[] = { 0x01, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
                                   0x8F, 0x03, 0xD0, 0x00, 0x04, 0x8A, 0x20, 0xD8,
                                   0x58, 0xFA, 0x39, 0xA0 };
    memcpy(copyr2, deb, sizeof(deb));
    uint8_t cc[4];
    tt_to_cchh(0, cc);
    memcpy(copyr2 + 22, cc, 4);                          /* extent start CCHH */
    tt_to_cchh((uint32_t)(ntracks - 1), cc);
    memcpy(copyr2 + 26, cc, 4);                          /* extent end CCHH */
    copyr2[30] = (ntracks >> 8) & 0xFF; copyr2[31] = ntracks & 0xFF;

    if (pdse) memcpy(copyr2, pdse_copyr2, sizeof(copyr2));
    if (netdata_writer_emit_data(writer, copyr2, sizeof(copyr2)) != 0) {
        log_error("Failed to emit COPYR2 record");
        free(members);
        free(dir_blocks);
        return RC_ERR_STREAM;
    }

    /* 4c. Directory blocks, one 276-byte unit each: count field (CCHHR zero, KL=8,
     * DL=256), the key (highest name in the block, X'FF' for the block holding the
     * end marker) and the 256-byte block. A zero count field ends the directory. */
    if (dir_used_count == 0) dir_used_count = dir_block_count;
    if (dir_used_count == 0) dir_used_count = 1;
    for (size_t b = 0; b < dir_used_count; b++) {
        uint8_t dirrec[276 + 12];
        memset(dirrec, 0, sizeof(dirrec));
        dirrec[9] = 0x08;
        dirrec[10] = 0x01; dirrec[11] = 0x00;
        if (pdse) dirrec[0] = 0x08;

        memset(dirrec + 12, 0xFF, 8);
        if (b < dir_block_count) {
            uint8_t *blk = dir_blocks[b];
            uint16_t used = (blk[0] << 8) | blk[1];
            if (used > 256) used = 256;
            size_t off = 2;
            while (off + 8 <= used) {
                memcpy(dirrec + 12, blk + off, 8);   /* the end marker itself (FF..) is the key of the last block */
                if (blk[off] == 0xFF || off + 12 > used) break;
                uint8_t c = blk[off + 11] & 0x1F;
                off += 12 + c * 2;
            }
            memcpy(dirrec + 20, blk, 256);
        }

        size_t reclen = 276 + (b + 1 == dir_used_count && !pdse ? 12 : 0);   /* trailing zero count after the last block */
        if (netdata_writer_emit_data(writer, dirrec, reclen) != 0) {
            log_error("Failed to emit directory record for block %zu", b + 1);
            free(members);
            free(dir_blocks);
            return RC_ERR_STREAM;
        }
    }

    if (pdse) {
        uint8_t term[12];
        memset(term, 0, sizeof(term));
        term[0] = PDSE_FLAG_END_DIR;
        if (netdata_writer_emit_data(writer, term, sizeof(term)) != 0) {
            log_error("Failed to emit directory terminator");
            free(members);
            free(dir_blocks);
            return RC_ERR_STREAM;
        }
    }

    /* Step 5: Read and emit member data. Each physical block is sent whole,
     * with its BDW/RDW for variable formats, behind a 12-byte header holding
     * the block's CCHHR and length; the member ends with a zero-length header. */
    uint8_t *block_buf = malloc(65536);
    uint8_t *pend_buf = malloc(65536 + 24);
    if (!block_buf || !pend_buf) {
        log_error("Out of memory allocating member record buffers");
        free(block_buf);
        free(pend_buf);
        free(members);
        free(dir_blocks);
        return RC_ERR_INTERNAL;
    }

    size_t total_member_blocks = 0;
    int stream_rc = 0;
    if (pdse && emit_pdse_members(writer, src_dsn, members, member_count, &total_member_blocks) != 0) {
        stream_rc = -1;
    }
    for (size_t m = 0; !pdse && m < member_count && stream_rc == 0; m++) {
        pds_member_entry_t *mbr = &members[m];
        if (mbr->is_alias) {
            /* Skip aliases during physical data streaming */
            continue;
        }

        snprintf(alloc_cmd, sizeof(alloc_cmd), "alloc fi(MBRDD) da('%s(%s)') shr",
                 src_dsn, mbr->name_ascii);
        rc = dynalloc_str(alloc_cmd, msg, sizeof(msg));
        if (rc != 0) {
            log_warn("Failed to allocate member '%s': %s (skipping)", mbr->name_ascii, msg);
            continue;
        }

        FILE *fmem = fopen("DD:MBRDD", "rb,type=blocked");
        if (!fmem) {
            log_warn("Failed to open member '%s': %s (skipping)", mbr->name_ascii, strerror(errno));
            dynalloc_str("free fi(MBRDD)", msg, sizeof(msg));
            continue;
        }

        /* Block addresses follow the directory TTR of the member and then fill
         * 3390 tracks the way the DASD does, so that no block of a member
         * carries the address another member starts at. */
        uint32_t tt = ((uint32_t)mbr->ttr[0] << 8) | mbr->ttr[1];
        unsigned r = mbr->ttr[2];
        uint32_t used = 0;           /* cells in use on the current track */
        size_t nread;
        size_t mbr_blocks = 0;
        size_t pend_len = 0;
        while ((nread = fread(block_buf, 1, 65536, fmem)) > 0) {
            if (pend_len > 0 && emit_unit(writer, pend_buf, pend_len) != 0) { stream_rc = -1; break; }
            uint32_t need = record_cells(nread);
            if (mbr_blocks == 0) {
                used = (r - 1) * need;              /* records before the member on its first track */
            } else if (used + need > TRACK_CELLS || r > 255) {
                tt++; r = 1; used = 0;
            }
            mbr_blocks++;
            total_member_blocks++;
            block_header(pend_buf, tt, (uint8_t)r++, nread);
            used += need;
            memcpy(pend_buf + 12, block_buf, nread);
            pend_len = 12 + nread;
        }

        fclose(fmem);
        dynalloc_str("free fi(MBRDD)", msg, sizeof(msg));

        if (stream_rc == 0) {
            /* 12-byte member EOF header (length 0). Like IEBCOPY, it rides with the
             * last block only while the record stays within the smaller of the
             * source BLKSIZE and the unload block size; otherwise (and for an
             * empty member) it is a record of its own. IEBCOPY's reload rejects
             * streams that attach it to a full-size block. */
            if (used + record_cells(0) > TRACK_CELLS || r > 255) { tt++; r = 1; }
            size_t eof_limit = blksize < UNLOAD_BLKSIZE ? blksize : UNLOAD_BLKSIZE;
            if (pend_len > 0 && pend_len + 12 > eof_limit) {
                if (emit_unit(writer, pend_buf, pend_len) != 0) stream_rc = -1;
                pend_len = 0;
            }
            if (stream_rc == 0) {
                block_header(pend_buf + pend_len, tt, (uint8_t)r, 0);
                pend_len += 12;
                if (emit_unit(writer, pend_buf, pend_len) != 0) stream_rc = -1;
            }
        }
        if (stream_rc != 0) {
            log_error("Failed to emit data for member '%s'", mbr->name_ascii);
            break;
        }

        log_verbose("Streamed member '%s' (%zu blocks)", mbr->name_ascii, mbr_blocks);
    }

    free(block_buf);
    free(pend_buf);
    if (stream_rc != 0) {
        free(members);
        free(dir_blocks);
        return RC_ERR_STREAM;
    }

    /* Step 6: Emit INMR06 Trailer Record */
    if (netdata_writer_emit_inmr06(writer) != 0) {
        log_error("Failed to emit INMR06 trailer record");
        free(members);
        free(dir_blocks);
        return RC_ERR_STREAM;
    }

    netdata_writer_flush(writer);

    log_info("Successfully unloaded PDS %s (%zu members, %zu total blocks)",
             src_dsn, member_count, total_member_blocks);

    free(members);
    free(dir_blocks);
    return RC_SUCCESS;
}

/* PDSE (data library or program library): the physical layout is not a PDS
 * track image, so the unload is made by IEBCOPY itself. It copies the PDSE to
 * a temporary unload data set (RECFM VS, 32756/3120) and the records of that
 * data set are the data records of the stream, as in TSO TRANSMIT. */
int xmit_pdse_stream(const ussxmit_opts_t *opts, const char *src_dsn, netdata_writer_t *writer)
{
    (void)opts;
    char msg[256];
    char cmd[320];
    char unload_dsn[64];
    static char iebcopy_log[16384];
    int result = RC_ERR_STREAM;

    log_verbose("Starting PDSE unload through IEBCOPY for: %s", src_dsn);

    dynalloc_temp_dsn(unload_dsn, sizeof(unload_dsn), "XMUNL");
    if (iebcopy_alloc_unload(unload_dsn, "UNLOAD", 0) != 0) return RC_ERR_ALLOC_SRC;

    /* touch the new data set so that it is formatted, then release the DD */
    FILE *ft = fopen("DD:UNLOAD", "wb,type=record");
    if (ft) fclose(ft);
    dynalloc_str("free fi(UNLOAD)", msg, sizeof(msg));
    if (!ft) {
        log_error("Cannot open unload data set '%s': %s", unload_dsn, strerror(errno));
        dynalloc_delete_dsn(unload_dsn);
        return RC_ERR_ALLOC_SRC;
    }

    int irc = iebcopy_copy(src_dsn, unload_dsn, iebcopy_log, sizeof(iebcopy_log));
    if (irc < 0 || irc > 4) {
        log_error("IEBCOPY unload of '%s' failed (rc=%d)", src_dsn, irc);
        if (iebcopy_log[0]) fprintf(stderr, "%s", iebcopy_log);
        dynalloc_delete_dsn(unload_dsn);
        return RC_ERR_TSO;
    }
    if (iebcopy_log[0]) log_verbose("IEBCOPY messages:\n%s", iebcopy_log);

    snprintf(cmd, sizeof(cmd), "alloc fi(UNLOAD) da('%s') shr", unload_dsn);
    if (dynalloc_str(cmd, msg, sizeof(msg)) != 0) {
        log_error("Failed to allocate unload data set '%s': %s", unload_dsn, msg);
        dynalloc_delete_dsn(unload_dsn);
        return RC_ERR_ALLOC_SRC;
    }
    FILE *fu = fopen("DD:UNLOAD", "rb,type=record");
    uint8_t *rec = malloc(IEBCOPY_UNLOAD_LRECL + 16);
    if (!fu || !rec) {
        log_error("Cannot open unload data set '%s': %s", unload_dsn, strerror(errno));
        if (fu) fclose(fu);
        free(rec);
        dynalloc_str("free fi(UNLOAD)", msg, sizeof(msg));
        dynalloc_delete_dsn(unload_dsn);
        return RC_ERR_ALLOC_SRC;
    }

    /* Pass 1: COPYR1 (attributes) and total size */
    size_t n = fread(rec, 1, IEBCOPY_UNLOAD_LRECL, fu);
    if (n < 56) {
        log_error("IEBCOPY unload of '%s' is empty or truncated", src_dsn);
        goto done;
    }
    uint8_t  c1_flags = rec[0];
    uint16_t c1_dsorg = (uint16_t)((rec[4] << 8) | rec[5]);
    uint32_t c1_blksize = (uint32_t)((rec[6] << 8) | rec[7]);
    uint32_t c1_lrecl = (uint32_t)((rec[8] << 8) | rec[9]);
    uint8_t  c1_recfm = rec[10];
    uint64_t size_bytes = n;
    while ((n = fread(rec, 1, IEBCOPY_UNLOAD_LRECL, fu)) > 0) size_bytes += n;
    rewind(fu);
    log_verbose("PDSE unload: %llu bytes, COPYR1 flags 0x%02X, DSORG 0x%04X, RECFM 0x%02X, LRECL %u, BLKSIZE %u",
                (unsigned long long)size_bytes, c1_flags, c1_dsorg, c1_recfm, c1_lrecl, c1_blksize);

    writer->inmtype = (c1_flags & 0x10) ? 0x40 : 0x80;
    if (netdata_writer_emit_inmr01(writer, "POK", get_current_hlq()) != 0 ||
        netdata_writer_emit_inmr02_ex(writer, "IEBCOPY", src_dsn, DSORG_PO, c1_recfm, 0x00, c1_lrecl,
                                      c1_blksize, size_bytes, 1) != 0 ||
        netdata_writer_emit_inmr02_ex(writer, "INMCOPY", NULL, DSORG_PS, 0x48, 0x02, IEBCOPY_UNLOAD_LRECL,
                                      IEBCOPY_UNLOAD_BLKSIZE, size_bytes, 0) != 0 ||
        netdata_writer_emit_inmr03(writer, DSORG_PS, 0x01, 80, size_bytes) != 0) {
        log_error("Failed to emit INMR01/INMR02/INMR03 records");
        goto done;
    }

    /* Pass 2: every unload record is a data record of the stream */
    size_t nrec = 0;
    while ((n = fread(rec, 1, IEBCOPY_UNLOAD_LRECL, fu)) > 0) {
        if (netdata_writer_emit_data(writer, rec, n) != 0) {
            log_error("Failed writing data segment for record %zu", nrec + 1);
            goto done;
        }
        nrec++;
    }
    if (netdata_writer_emit_inmr06(writer) != 0) {
        log_error("Failed to emit INMR06 trailer record");
        goto done;
    }
    netdata_writer_flush(writer);
    log_info("Successfully unloaded PDSE %s (%zu records, %llu bytes)", src_dsn, nrec,
             (unsigned long long)size_bytes);
    result = RC_SUCCESS;

done:
    fclose(fu);
    free(rec);
    dynalloc_str("free fi(UNLOAD)", msg, sizeof(msg));
    dynalloc_delete_dsn(unload_dsn);
    return result;
}
