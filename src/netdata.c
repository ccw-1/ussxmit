#define _EXT 1
#define _XOPEN_SOURCE_EXTENDED 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>

#include "netdata.h"
#include "utils.h"

static const uint8_t inmr01_eye[6] = {0xC9, 0xD5, 0xD4, 0xD9, 0xF0, 0xF1};
static const uint8_t inmr02_eye[6] = {0xC9, 0xD5, 0xD4, 0xD9, 0xF0, 0xF2};
static const uint8_t inmr03_eye[6] = {0xC9, 0xD5, 0xD4, 0xD9, 0xF0, 0xF3};
static const uint8_t inmr06_eye[6] = {0xC9, 0xD5, 0xD4, 0xD9, 0xF0, 0xF6};

static uint16_t read_u16_be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t read_u32_be(const uint8_t *p, size_t len)
{
    uint32_t val = 0;
    for (size_t i = 0; i < len && i < 4; i++) {
        val = (val << 8) | (uint32_t)p[i];
    }
    return val;
}

static void write_u16_be(uint8_t *p, uint16_t val)
{
    p[0] = (uint8_t)(val >> 8);
    p[1] = (uint8_t)(val & 0xFF);
}

static void write_u32_be(uint8_t *p, uint32_t val)
{
    p[0] = (uint8_t)(val >> 24);
    p[1] = (uint8_t)((val >> 16) & 0xFF);
    p[2] = (uint8_t)((val >> 8) & 0xFF);
    p[3] = (uint8_t)(val & 0xFF);
}

/* ================================================================== */
/* Netdata Stream Writer (XMIT Encoder)                               */
/* ================================================================== */

void netdata_writer_init(netdata_writer_t *w, FILE *out_fp)
{
    if (!w) return;
    w->out_fp = out_fp;
    w->card_pos = 0;
    w->total_records_written = 0;
    w->total_bytes_written = 0;
    memset(w->card_buf, 0x40, sizeof(w->card_buf));
}

int netdata_writer_put_segment(netdata_writer_t *w, uint8_t flags, const uint8_t *data, size_t len)
{
    if (!w || !w->out_fp || len > 253) return -1;

    uint8_t seg_hdr[2];
    seg_hdr[0] = (uint8_t)(len + 2);
    seg_hdr[1] = flags;

    for (int h = 0; h < 2; h++) {
        w->card_buf[w->card_pos++] = seg_hdr[h];
        if (w->card_pos == 80) {
            if (fwrite(w->card_buf, 1, 80, w->out_fp) != 80) return -1;
            w->total_records_written++;
            w->total_bytes_written += 80;
            w->card_pos = 0;
            memset(w->card_buf, 0x40, sizeof(w->card_buf));
        }
    }

    for (size_t i = 0; i < len; i++) {
        w->card_buf[w->card_pos++] = data[i];
        if (w->card_pos == 80) {
            if (fwrite(w->card_buf, 1, 80, w->out_fp) != 80) return -1;
            w->total_records_written++;
            w->total_bytes_written += 80;
            w->card_pos = 0;
            memset(w->card_buf, 0x40, sizeof(w->card_buf));
        }
    }

    return 0;
}

int netdata_writer_emit_record(netdata_writer_t *w, bool is_control, const uint8_t *data, size_t len)
{
    if (!w || !w->out_fp) return -1;

    if (len <= 253) {
        uint8_t flags = is_control ? 0xE0 : 0xC0;
        return netdata_writer_put_segment(w, flags, data, len);
    }

    /* Spanned logical record (len > 253) */
    size_t offset = 0;
    bool first = true;
    while (offset < len) {
        size_t rem = len - offset;
        size_t chunk = (rem > 253) ? 253 : rem;
        bool last = (offset + chunk >= len);

        uint8_t flags;
        if (first) {
            flags = is_control ? 0xA0 : 0x80;
            first = false;
        } else if (last) {
            flags = is_control ? 0x60 : 0x40;
        } else {
            flags = is_control ? 0x20 : 0x00;
        }

        if (netdata_writer_put_segment(w, flags, data + offset, chunk) != 0) {
            return -1;
        }
        offset += chunk;
    }

    return 0;
}

int netdata_writer_emit_inmr01(netdata_writer_t *w, const char *node, const char *user)
{
    uint8_t buf[256];
    size_t off = 0;

    /* 6-byte eyecatcher INMR01 */
    memcpy(buf + off, inmr01_eye, 6);
    off += 6;

    /* TU 0x0042 (1 byte, 0x50) */
    write_u16_be(buf + off, 0x0042); off += 2;
    write_u16_be(buf + off, 1);      off += 2;
    write_u16_be(buf + off, 1);      off += 2;
    buf[off++] = 0x50;

    /* TU 0x1011: Origin Node */
    char origin_node[16];
    if (!node || !*node) node = "POK";
    strncpy(origin_node, node, sizeof(origin_node) - 1);
    origin_node[sizeof(origin_node) - 1] = '\0';
    a2e_str(origin_node);
    size_t nlen = strlen(node);
    if (nlen > 8) nlen = 8;
    write_u16_be(buf + off, 0x1011); off += 2;
    write_u16_be(buf + off, 1);      off += 2;
    write_u16_be(buf + off, (uint16_t)nlen); off += 2;
    memcpy(buf + off, origin_node, nlen); off += nlen;

    /* TU 0x1012: Origin User */
    char origin_user[16];
    if (!user || !*user) user = get_current_hlq();
    strncpy(origin_user, user, sizeof(origin_user) - 1);
    origin_user[sizeof(origin_user) - 1] = '\0';
    a2e_str(origin_user);
    size_t ulen = strlen(user);
    if (ulen > 8) ulen = 8;
    write_u16_be(buf + off, 0x1012); off += 2;
    write_u16_be(buf + off, 1);      off += 2;
    write_u16_be(buf + off, (uint16_t)ulen); off += 2;
    memcpy(buf + off, origin_user, ulen); off += ulen;

    /* TU 0x1001: Target Node */
    write_u16_be(buf + off, 0x1001); off += 2;
    write_u16_be(buf + off, 1);      off += 2;
    write_u16_be(buf + off, 5);      off += 2;
    char dummy_node[] = "DUMMY";
    a2e_str(dummy_node);
    memcpy(buf + off, dummy_node, 5); off += 5;

    /* TU 0x1002: Target User */
    write_u16_be(buf + off, 0x1002); off += 2;
    write_u16_be(buf + off, 1);      off += 2;
    write_u16_be(buf + off, 5);      off += 2;
    char dummy_user[] = "DUMMY";
    a2e_str(dummy_user);
    memcpy(buf + off, dummy_user, 5); off += 5;

    /* TU 0x1024: Timestamp (YYYYMMDDHHMMSS) */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[16];
    snprintf(ts, sizeof(ts), "%04d%02d%02d%02d%02d%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
    a2e_str(ts);
    write_u16_be(buf + off, 0x1024); off += 2;
    write_u16_be(buf + off, 1);      off += 2;
    write_u16_be(buf + off, 14);     off += 2;
    memcpy(buf + off, ts, 14); off += 14;

    /* TU 0x102F (1 byte, 0x01) */
    write_u16_be(buf + off, 0x102F); off += 2;
    write_u16_be(buf + off, 1);      off += 2;
    write_u16_be(buf + off, 1);      off += 2;
    buf[off++] = 0x01;

    return netdata_writer_emit_record(w, true, buf, off);
}

int netdata_writer_emit_inmr02(netdata_writer_t *w, const char *dsn, uint16_t dsorg, uint8_t recfm, uint32_t lrecl, uint32_t blksize)
{
    uint8_t buf[256];
    size_t off = 0;

    /* 6-byte eyecatcher INMR02 */
    memcpy(buf + off, inmr02_eye, 6);
    off += 6;

    /* 4-byte file number: 0x00000001 */
    write_u32_be(buf + off, 1);
    off += 4;

    /* TU 0x1028: Utility Name (INMCOPY) */
    char util[16] = "INMCOPY";
    a2e_str(util);
    write_u16_be(buf + off, 0x1028); off += 2;
    write_u16_be(buf + off, 1);      off += 2;
    write_u16_be(buf + off, 7);      off += 2;
    memcpy(buf + off, util, 7); off += 7;

    /* TU 0x102C: Flags (0x00 0x01 0x80 0x00) */
    write_u16_be(buf + off, 0x102C); off += 2;
    write_u16_be(buf + off, 1);      off += 2;
    write_u16_be(buf + off, 4);      off += 2;
    buf[off++] = 0x00; buf[off++] = 0x01; buf[off++] = 0x80; buf[off++] = 0x00;

    /* TU 0x003C: DSORG (2 bytes) */
    write_u16_be(buf + off, 0x003C); off += 2;
    write_u16_be(buf + off, 1);      off += 2;
    write_u16_be(buf + off, 2);      off += 2;
    write_u16_be(buf + off, dsorg);  off += 2;

    /* TU 0x0042: LRECL (4 bytes) */
    write_u16_be(buf + off, 0x0042); off += 2;
    write_u16_be(buf + off, 1);      off += 2;
    write_u16_be(buf + off, 4);      off += 2;
    write_u32_be(buf + off, lrecl);  off += 4;

    /* TU 0x0030: BLKSIZE (4 bytes) */
    write_u16_be(buf + off, 0x0030); off += 2;
    write_u16_be(buf + off, 1);      off += 2;
    write_u16_be(buf + off, 4);      off += 2;
    write_u32_be(buf + off, blksize ? blksize : lrecl); off += 4;

    /* TU 0x0049: RECFM (2 bytes: recfm flags + 0x02) */
    write_u16_be(buf + off, 0x0049); off += 2;
    write_u16_be(buf + off, 1);      off += 2;
    write_u16_be(buf + off, 2);      off += 2;
    buf[off++] = recfm;
    buf[off++] = 0x02;

    /* TU 0x0002: INMDSNAM (split qualifiers) */
    char norm_dsn[64];
    dsn_normalize(norm_dsn, sizeof(norm_dsn), dsn);

    char *quals[16];
    int qcnt = 0;
    char dsn_copy[64];
    strncpy(dsn_copy, norm_dsn, sizeof(dsn_copy) - 1);
    dsn_copy[sizeof(dsn_copy) - 1] = '\0';

    char *token = strtok(dsn_copy, ".");
    while (token && qcnt < 16) {
        quals[qcnt++] = token;
        token = strtok(NULL, ".");
    }

    if (qcnt > 0) {
        write_u16_be(buf + off, 0x0002); off += 2;
        write_u16_be(buf + off, (uint16_t)qcnt); off += 2;
        for (int q = 0; q < qcnt; q++) {
            size_t ql = strlen(quals[q]);
            char qe[16];
            strncpy(qe, quals[q], sizeof(qe) - 1);
            qe[sizeof(qe) - 1] = '\0';
            a2e_str(qe);
            write_u16_be(buf + off, (uint16_t)ql); off += 2;
            memcpy(buf + off, qe, ql); off += ql;
        }
    }

    return netdata_writer_emit_record(w, true, buf, off);
}

int netdata_writer_emit_inmr03(netdata_writer_t *w, uint16_t dsorg, uint8_t recfm, uint32_t lrecl)
{
    uint8_t buf[128];
    size_t off = 0;

    memcpy(buf + off, inmr03_eye, 6);
    off += 6;

    write_u16_be(buf + off, 0x102C); off += 2;
    write_u16_be(buf + off, 1);      off += 2;
    write_u16_be(buf + off, 4);      off += 2;
    buf[off++] = 0x00; buf[off++] = 0x01; buf[off++] = 0x80; buf[off++] = 0x00;

    write_u16_be(buf + off, 0x003C); off += 2;
    write_u16_be(buf + off, 1);      off += 2;
    write_u16_be(buf + off, 2);      off += 2;
    write_u16_be(buf + off, dsorg);  off += 2;

    write_u16_be(buf + off, 0x0042); off += 2;
    write_u16_be(buf + off, 1);      off += 2;
    write_u16_be(buf + off, 2);      off += 2;
    write_u16_be(buf + off, (uint16_t)lrecl); off += 2;

    write_u16_be(buf + off, 0x0049); off += 2;
    write_u16_be(buf + off, 1);      off += 2;
    write_u16_be(buf + off, 2);      off += 2;
    buf[off++] = 0x00; buf[off++] = (uint8_t)(recfm & 0x01);

    return netdata_writer_emit_record(w, true, buf, off);
}

int netdata_writer_emit_data(netdata_writer_t *w, const uint8_t *data, size_t len)
{
    return netdata_writer_emit_record(w, false, data, len);
}

int netdata_writer_emit_inmr06(netdata_writer_t *w)
{
    uint8_t buf[8];
    memcpy(buf, inmr06_eye, 6);
    return netdata_writer_emit_record(w, true, buf, 6);
}

int netdata_writer_flush(netdata_writer_t *w)
{
    if (!w || !w->out_fp) return -1;

    if (w->card_pos > 0) {
        /* Pad remaining bytes in 80-byte card with EBCDIC space 0x40 */
        memset(w->card_buf + w->card_pos, 0x40, 80 - w->card_pos);
        if (fwrite(w->card_buf, 1, 80, w->out_fp) != 80) return -1;
        w->total_records_written++;
        w->total_bytes_written += 80;
        w->card_pos = 0;
    }
    fflush(w->out_fp);
    return 0;
}

/* ================================================================== */
/* Netdata Parser (RECEIVE / Inspect Decoder)                         */
/* ================================================================== */

void netdata_parse_text_units(const uint8_t *p, size_t len, netdata_info_t *info, bool is_inmr02)
{
    size_t offset = 0;

    while (offset + 4 <= len) {
        uint16_t key   = read_u16_be(p + offset);
        uint16_t count = read_u16_be(p + offset + 2);
        offset += 4;

        if (count == 0) continue;

        for (uint16_t c = 0; c < count && offset + 2 <= len; c++) {
            uint16_t tlen = read_u16_be(p + offset);
            offset += 2;

            if (offset + tlen > len) break;

            const uint8_t *val = p + offset;

            if (!is_inmr02) {
                /* INMR01 text units */
                if (key == 0x1011 && tlen < sizeof(info->origin_node)) {
                    memcpy(info->origin_node, val, tlen);
                    info->origin_node[tlen] = '\0';
                    e2a_str(info->origin_node);
                } else if (key == 0x1012 && tlen < sizeof(info->origin_user)) {
                    memcpy(info->origin_user, val, tlen);
                    info->origin_user[tlen] = '\0';
                    e2a_str(info->origin_user);
                } else if (key == 0x1001 && tlen < sizeof(info->target_node)) {
                    memcpy(info->target_node, val, tlen);
                    info->target_node[tlen] = '\0';
                    e2a_str(info->target_node);
                } else if (key == 0x1002 && tlen < sizeof(info->target_user)) {
                    memcpy(info->target_user, val, tlen);
                    info->target_user[tlen] = '\0';
                    e2a_str(info->target_user);
                } else if (key == 0x1024 && tlen < sizeof(info->timestamp)) {
                    char raw_ts[32];
                    size_t rlen = tlen < sizeof(raw_ts) - 1 ? tlen : sizeof(raw_ts) - 1;
                    memcpy(raw_ts, val, rlen);
                    raw_ts[rlen] = '\0';
                    e2a_str(raw_ts);
                    if (rlen >= 14) {
                        snprintf(info->timestamp, sizeof(info->timestamp),
                                 "%.4s-%.2s-%.2s %.2s:%.2s:%.2s",
                                 raw_ts, raw_ts + 4, raw_ts + 6,
                                 raw_ts + 8, raw_ts + 10, raw_ts + 12);
                    } else {
                        strncpy(info->timestamp, raw_ts, sizeof(info->timestamp) - 1);
                    }
                } else if (key == INMUTILN_KEY && tlen < sizeof(info->utility_name)) {
                    memcpy(info->utility_name, val, tlen);
                    info->utility_name[tlen] = '\0';
                    e2a_str(info->utility_name);
                }
            } else {
                /* INMR02 text units */
                if (key == INMDSNAM_KEY) {
                    char qual[64];
                    size_t qlen = tlen < sizeof(qual) - 1 ? tlen : sizeof(qual) - 1;
                    memcpy(qual, val, qlen);
                    qual[qlen] = '\0';
                    e2a_str(qual);

                    if (info->dataset_name[0] != '\0') {
                        strncat(info->dataset_name, ".", sizeof(info->dataset_name) - strlen(info->dataset_name) - 1);
                    }
                    strncat(info->dataset_name, qual, sizeof(info->dataset_name) - strlen(info->dataset_name) - 1);
                } else if (key == INMMBR_KEY && tlen < sizeof(info->member_name)) {
                    memcpy(info->member_name, val, tlen);
                    info->member_name[tlen] = '\0';
                    e2a_str(info->member_name);
                } else if (key == INMDIR_KEY) {
                    info->dir_blocks = read_u32_be(val, tlen);
                } else if (key == INMDSORG_KEY && tlen >= 2) {
                    info->dsorg = read_u16_be(val);
                } else if (key == INMLRECL_KEY) {
                    info->lrecl = read_u32_be(val, tlen);
                } else if (key == INMRECFM_KEY && tlen >= 1) {
                    info->recfm = val[0];
                } else if (key == INMBLKSIZE_KEY) {
                    info->blksize = read_u32_be(val, tlen);
                } else if (key == 0x1028 && tlen < sizeof(info->utility_name)) {
                    memcpy(info->utility_name, val, tlen);
                    info->utility_name[tlen] = '\0';
                    e2a_str(info->utility_name);
                }
            }

            offset += tlen;
        }
    }
}

void netdata_reader_init(netdata_reader_t *r, netdata_record_cb_t cb, void *user_data)
{
    if (!r) return;
    memset(r, 0, sizeof(*r));
    r->record_cb = cb;
    r->user_data = user_data;
}

int netdata_reader_feed(netdata_reader_t *r, const uint8_t *buf, size_t len)
{
    if (!r || !buf) return -1;

    size_t in_pos = 0;

    while (in_pos < len || r->stream_len > 0) {
        /* Fill stream buffer from input */
        while (in_pos < len && r->stream_len < sizeof(r->stream_buf)) {
            r->stream_buf[r->stream_len++] = buf[in_pos++];
        }

        if (r->stream_len < 2) {
            break; /* Need more input */
        }

        uint8_t seg_len = r->stream_buf[0];
        uint8_t flags   = r->stream_buf[1];

        if (seg_len < 2) {
            /* Skip padding byte (0x00 or 0x40) */
            memmove(r->stream_buf, r->stream_buf + 1, r->stream_len - 1);
            r->stream_len--;
            continue;
        }

        if (r->stream_len < seg_len) {
            if (in_pos >= len) {
                /* Incomplete segment, wait for more data */
                break;
            }
            continue;
        }

        const uint8_t *seg_data = r->stream_buf + 2;
        size_t seg_data_len = seg_len - 2;

        bool is_first = (flags & 0x80) != 0;
        bool is_last  = (flags & 0x40) != 0;
        bool is_ctl   = (flags & 0x20) != 0;

        if (is_first) {
            r->assembly_len = 0;
            r->assembly_is_control = is_ctl;
            r->in_spanned_record = !is_last;
        }

        if (r->assembly_len + seg_data_len <= sizeof(r->assembly_buf)) {
            memcpy(r->assembly_buf + r->assembly_len, seg_data, seg_data_len);
            r->assembly_len += seg_data_len;
        }

        if (is_last) {
            r->in_spanned_record = false;
            r->total_records_dispatched++;
            if (r->record_cb) {
                int rc = r->record_cb(r->assembly_is_control, r->assembly_buf, r->assembly_len, r->user_data);
                if (rc != 0) return rc;
            }
            r->assembly_len = 0;
        }

        /* Consume segment from stream buffer */
        memmove(r->stream_buf, r->stream_buf + seg_len, r->stream_len - seg_len);
        r->stream_len -= seg_len;
    }

    return 0;
}

int netdata_reader_feed_card(netdata_reader_t *r, const uint8_t card[80])
{
    return netdata_reader_feed(r, card, 80);
}

static int header_parse_cb(bool is_control, const uint8_t *data, size_t len, void *user_data)
{
    netdata_info_t *info = (netdata_info_t *)user_data;
    if (!is_control || len < 6) return 0;

    if (memcmp(data, inmr01_eye, 6) == 0) {
        info->has_inmr01 = true;
        if (len > 6) {
            netdata_parse_text_units(data + 6, len - 6, info, false);
        }
    } else if (memcmp(data, inmr02_eye, 6) == 0) {
        info->has_inmr02 = true;
        if (len > 10) {
            netdata_parse_text_units(data + 10, len - 10, info, true);
        }
    } else if (memcmp(data, inmr06_eye, 6) == 0) {
        return 1; /* Stop on trailer */
    }

    return 0;
}

int netdata_parse_header(const uint8_t *buffer, size_t buflen, netdata_info_t *info)
{
    if (!buffer || buflen < 80 || !info) return -1;
    memset(info, 0, sizeof(*info));

    netdata_reader_t reader;
    netdata_reader_init(&reader, header_parse_cb, info);

    size_t offset = 0;
    while (offset + 80 <= buflen) {
        int rc = netdata_reader_feed_card(&reader, buffer + offset);
        if (rc > 0 || (info->has_inmr01 && info->has_inmr02)) {
            break;
        }
        if (rc < 0) return -1;
        offset += 80;
    }

    return (info->has_inmr01 && info->has_inmr02) ? 0 : -1;
}

void netdata_format_dsorg(uint16_t dsorg, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    if (dsorg & DSORG_PO) {
        snprintf(out, out_sz, "PO (Partitioned)");
    } else if (dsorg & DSORG_PS) {
        snprintf(out, out_sz, "PS (Sequential)");
    } else if (dsorg & DSORG_DA) {
        snprintf(out, out_sz, "DA (Direct)");
    } else {
        snprintf(out, out_sz, "Unknown (0x%04X)", dsorg);
    }
}

void netdata_format_recfm(uint8_t recfm, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    char buf[16] = {0};
    int pos = 0;

    if ((recfm & RECFM_U) == RECFM_U) {
        buf[pos++] = 'U';
    } else if (recfm & RECFM_F) {
        buf[pos++] = 'F';
    } else if (recfm & RECFM_V) {
        buf[pos++] = 'V';
    }

    if (recfm & RECFM_B) buf[pos++] = 'B';
    if (recfm & RECFM_S) buf[pos++] = 'S';
    if (recfm & RECFM_A) buf[pos++] = 'A';
    if (recfm & RECFM_M) buf[pos++] = 'M';

    buf[pos] = '\0';
    snprintf(out, out_sz, "%s", buf);
}
