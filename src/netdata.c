#define _EXT 1
#define _XOPEN_SOURCE_EXTENDED 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "netdata.h"
#include "utils.h"

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

/* Parse Text Units from a control record buffer (EBCDIC) */
static void parse_text_units(const uint8_t *p, size_t len, netdata_info_t *info, bool is_inmr02)
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
                    /* Format: YYYYMMDDHHMMSS */
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
                    /* Utility name in INMR02 e.g. INMCOPY, IEBCOPY */
                    memcpy(info->utility_name, val, tlen);
                    info->utility_name[tlen] = '\0';
                    e2a_str(info->utility_name);
                }
            }

            offset += tlen;
        }
    }
}

int netdata_parse_header(const uint8_t *buffer, size_t buflen, netdata_info_t *info)
{
    if (!buffer || buflen < 80 || !info) return -1;
    memset(info, 0, sizeof(*info));

    size_t offset = 0;

    while (offset + 2 <= buflen) {
        uint8_t seg_len = buffer[offset];
        uint8_t flags   = buffer[offset + 1];

        if (seg_len < 2) break;
        if (offset + seg_len > buflen) break;

        const uint8_t *payload = buffer + offset + 2;
        size_t payload_len = seg_len - 2;

        /* Control segment if flags & 0x20 == 0 */
        if ((flags & 0x20) == 0 && payload_len >= 6) {
            if (memcmp(payload, INMR01_EYECATCHER, 6) == 0) {
                info->has_inmr01 = true;
                parse_text_units(payload + 6, payload_len - 6, info, false);
            } else if (memcmp(payload, INMR02_EYECATCHER, 6) == 0) {
                info->has_inmr02 = true;
                parse_text_units(payload + 6, payload_len - 6, info, true);
            } else if (memcmp(payload, INMR06_EYECATCHER, 6) == 0) {
                break;
            }
        }

        offset += seg_len;
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
