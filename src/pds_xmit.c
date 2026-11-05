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
#include "utils.h"

#define MAX_MEMBERS 4096
#define MAX_DIR_BLOCKS 1024

typedef struct {
    char        name_ascii[9];
    uint8_t     name_ebcdic[8];
    uint8_t     ttr[3];
    uint8_t     c_byte;
    uint8_t     userdata[64];
    size_t      userdata_len;
    bool        is_alias;
} pds_member_entry_t;

int xmit_pds_stream(const ussxmit_opts_t *opts, const char *src_dsn,
                    uint16_t dsorg, uint8_t recfm, uint32_t lrecl, uint32_t blksize,
                    netdata_writer_t *writer)
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
    size_t n;
    while (dir_block_count < MAX_DIR_BLOCKS &&
           (n = fread(dir_blocks[dir_block_count], 1, 256, fdir)) > 0) {
        dir_block_count++;
        /* Check if last block ends with high key 0xFF... */
        uint8_t *blk = dir_blocks[dir_block_count - 1];
        if (n >= 2) {
            uint16_t used = (blk[0] << 8) | blk[1];
            if (used >= 10 && memcmp(blk + 2, "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8) == 0) {
                break;
            }
        }
    }

    fclose(fdir);
    dynalloc_str("free fi(PDSDIR)", msg, sizeof(msg));

    log_verbose("Read %zu directory blocks from %s", dir_block_count, src_dsn);

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
                    if (blksize == 0) blksize = (lrecl > 0) ? lrecl : 32760;
                    log_verbose("Refined PDS member attributes: RECFM=0x%02X, LRECL=%u, BLKSIZE=%u",
                                recfm, lrecl, blksize);
                }
                fclose(fprobe);
            }
            dynalloc_str("free fi(PROBEDD)", probe_msg, sizeof(probe_msg));
        }
    }

    /* Step 3: Emit Netdata Transmission Control Records */
    /* INMR01 Transmission Header */
    if (netdata_writer_emit_inmr01(writer, "POK", get_current_hlq()) != 0) {
        log_error("Failed to emit INMR01 record");
        free(members);
        free(dir_blocks);
        return RC_ERR_STREAM;
    }

    /* INMR02 Dataset Attributes */
    if (netdata_writer_emit_inmr02(writer, src_dsn, dsorg, recfm, lrecl, blksize) != 0) {
        log_error("Failed to emit INMR02 record");
        free(members);
        free(dir_blocks);
        return RC_ERR_STREAM;
    }

    /* Step 4: Emit IEBCOPY Unloaded Dataset Header & Directory Records */
    /* 4a. COPYR1 Header (56 bytes) */
    uint8_t copyr1[56];
    memset(copyr1, 0, sizeof(copyr1));
    copyr1[0] = 0x00;
    copyr1[1] = 0xCA; copyr1[2] = 0x6D; copyr1[3] = 0x0F;
    copyr1[4] = (dsorg >> 8) & 0xFF; copyr1[5] = dsorg & 0xFF;
    copyr1[6] = (blksize >> 8) & 0xFF; copyr1[7] = blksize & 0xFF;
    copyr1[8] = (lrecl >> 8) & 0xFF; copyr1[9] = lrecl & 0xFF;
    copyr1[10] = recfm;
    copyr1[11] = 0x00;
    copyr1[12] = 0x00; copyr1[13] = 0x80;
    copyr1[14] = (blksize >> 8) & 0xFF; copyr1[15] = blksize & 0xFF;

    static const uint8_t geom[] = {
        0x30, 0x30, 0x20, 0x0F, 0x00, 0x00, 0x7F, 0xF8,
        0x7F, 0xF8, 0x00, 0x0F, 0xE5, 0xA2, 0x00, 0x00,
        0x22, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x7E,
        0x01, 0x34, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x0E, 0xB3, 0xB2, 0x00, 0x00
    };
    memcpy(copyr1 + 16, geom, sizeof(geom));

    if (netdata_writer_emit_data(writer, copyr1, sizeof(copyr1)) != 0) {
        log_error("Failed to emit COPYR1 header record");
        free(members);
        free(dir_blocks);
        return RC_ERR_STREAM;
    }

    /* 4b. Record #6: First Directory Block / Device header (276 bytes) */
    uint8_t rec6[276];
    memset(rec6, 0, sizeof(rec6));
    rec6[0] = 0x01; rec6[1] = 0x00; rec6[2] = 0x00; rec6[3] = 0x00;
    rec6[4] = 0xFF; rec6[5] = 0x00; rec6[6] = 0x00; rec6[7] = 0x00;
    rec6[8] = 0x8F; rec6[9] = 0x03; rec6[10] = 0xD0; rec6[11] = 0x00;
    /* Dummy key */
    rec6[12] = 0x04; rec6[13] = 0x3B; rec6[14] = 0x50; rec6[15] = 0xD8;
    rec6[16] = 0x58; rec6[17] = 0xCF; rec6[18] = 0x7C; rec6[19] = 0x50;
    /* 256-byte track pointer */
    rec6[20] = 0x00; rec6[21] = 0x00; rec6[22] = 0x0C; rec6[23] = 0x0D;
    rec6[24] = 0x00; rec6[25] = 0x00; rec6[26] = 0x0C; rec6[27] = 0x0D;
    rec6[28] = 0x00; rec6[29] = 0x00; rec6[30] = 0x00; rec6[31] = 0x01;

    if (netdata_writer_emit_data(writer, rec6, sizeof(rec6)) != 0) {
        log_error("Failed to emit directory header record #6");
        free(members);
        free(dir_blocks);
        return RC_ERR_STREAM;
    }

    /* 4c. Record #7+: Directory Block Records (288 bytes each) */
    for (size_t b = 0; b < (dir_block_count > 0 ? dir_block_count : 1); b++) {
        uint8_t dirrec[288];
        memset(dirrec, 0, sizeof(dirrec));
        dirrec[8] = 0x00; dirrec[9] = 0x08;
        dirrec[10] = 0x01; dirrec[11] = 0x00;

        /* Find highest key in this block or default to 0xFF */
        memset(dirrec + 12, 0xFF, 8);
        if (dir_block_count > 0) {
            uint8_t *blk = dir_blocks[b];
            uint16_t used = (blk[0] << 8) | blk[1];
            if (used > 256) used = 256;
            size_t off = 2;
            while (off + 12 <= used) {
                if (blk[off] == 0xFF) break;
                memcpy(dirrec + 12, blk + off, 8);
                uint8_t c = blk[off + 11] & 0x1F;
                off += 12 + c * 2;
            }
            memcpy(dirrec + 20, blk, 256);
        }

        if (netdata_writer_emit_data(writer, dirrec, sizeof(dirrec)) != 0) {
            log_error("Failed to emit directory record for block %zu", b + 1);
            free(members);
            free(dir_blocks);
            return RC_ERR_STREAM;
        }
    }

    /* Step 5: Read and emit member data blocks */
    uint16_t track = 0x0C0D;
    uint8_t rec_num = 1;

    uint8_t *block_buf = malloc(65536);
    uint8_t *ckd_buf = malloc(65536 + 24);
    if (!block_buf || !ckd_buf) {
        log_error("Out of memory allocating member record buffers");
        free(block_buf);
        free(ckd_buf);
        free(members);
        free(dir_blocks);
        return RC_ERR_INTERNAL;
    }

    size_t total_member_blocks = 0;
    for (size_t m = 0; m < member_count; m++) {
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

        FILE *fmem = fopen("DD:MBRDD", "rb,type=record");
        if (!fmem) {
            log_warn("Failed to open member '%s': %s (skipping)", mbr->name_ascii, strerror(errno));
            dynalloc_str("free fi(MBRDD)", msg, sizeof(msg));
            continue;
        }

        size_t nread;
        size_t mbr_blocks = 0;
        while ((nread = fread(block_buf, 1, 65536, fmem)) > 0) {
            mbr_blocks++;
            total_member_blocks++;

            /* 12-byte CKD block header */
            ckd_buf[0] = 0; ckd_buf[1] = 0; ckd_buf[2] = 0; ckd_buf[3] = 0;
            ckd_buf[4] = (track >> 8) & 0xFF; ckd_buf[5] = track & 0xFF;
            ckd_buf[6] = (track >> 8) & 0xFF; ckd_buf[7] = track & 0xFF;
            ckd_buf[8] = rec_num++;
            ckd_buf[9] = (nread >> 16) & 0xFF;
            ckd_buf[10] = (nread >> 8) & 0xFF;
            ckd_buf[11] = nread & 0xFF;

            memcpy(ckd_buf + 12, block_buf, nread);
            if (netdata_writer_emit_data(writer, ckd_buf, 12 + nread) != 0) {
                log_error("Failed to emit member block for '%s'", mbr->name_ascii);
                fclose(fmem);
                dynalloc_str("free fi(MBRDD)", msg, sizeof(msg));
                free(block_buf);
                free(ckd_buf);
                free(members);
                free(dir_blocks);
                return RC_ERR_STREAM;
            }
        }

        fclose(fmem);
        dynalloc_str("free fi(MBRDD)", msg, sizeof(msg));

        /* 12-byte Member EOF record (length = 0) */
        ckd_buf[0] = 0; ckd_buf[1] = 0; ckd_buf[2] = 0; ckd_buf[3] = 0;
        ckd_buf[4] = (track >> 8) & 0xFF; ckd_buf[5] = track & 0xFF;
        ckd_buf[6] = (track >> 8) & 0xFF; ckd_buf[7] = track & 0xFF;
        ckd_buf[8] = rec_num++;
        ckd_buf[9] = 0; ckd_buf[10] = 0; ckd_buf[11] = 0;

        if (netdata_writer_emit_data(writer, ckd_buf, 12) != 0) {
            log_error("Failed to emit EOF marker for member '%s'", mbr->name_ascii);
            free(block_buf);
            free(ckd_buf);
            free(members);
            free(dir_blocks);
            return RC_ERR_STREAM;
        }

        log_verbose("Streamed member '%s' (%zu blocks)", mbr->name_ascii, mbr_blocks);
    }

    /* Step 6: Emit INMR06 Trailer Record */
    if (netdata_writer_emit_inmr06(writer) != 0) {
        log_error("Failed to emit INMR06 trailer record");
        free(block_buf);
        free(ckd_buf);
        free(members);
        free(dir_blocks);
        return RC_ERR_STREAM;
    }

    netdata_writer_flush(writer);

    log_info("Successfully unloaded PDS %s (%zu members, %zu total blocks)",
             src_dsn, member_count, total_member_blocks);

    free(block_buf);
    free(ckd_buf);
    free(members);
    free(dir_blocks);
    return RC_SUCCESS;
}
