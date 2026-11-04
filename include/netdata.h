#ifndef NETDATA_H
#define NETDATA_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Netdata Control Record Eyecatchers (EBCDIC) */
#define INMR01_EYECATCHER "\xC9\xD5\xD4\xD9\xF0\xF1"  /* INMR01: Transmission Header */
#define INMR02_EYECATCHER "\xC9\xD5\xD4\xD9\xF0\xF2"  /* INMR02: Dataset Utility / Attributes */
#define INMR03_EYECATCHER "\xC9\xD5\xD4\xD9\xF0\xF3"  /* INMR03: User Control Record */
#define INMR04_EYECATCHER "\xC9\xD5\xD4\xD9\xF0\xF4"  /* INMR04: Data Record */
#define INMR06_EYECATCHER "\xC9\xD5\xD4\xD9\xF0\xF6"  /* INMR06: Trailer / End of Transmission */
#define INMR07_EYECATCHER "\xC9\xD5\xD4\xD9\xF0\xF7"  /* INMR07: Acknowledgment Record */

/* Netdata Text Unit Keys */
#define INMDSNAM_KEY    0x0002  /* Dataset name qualifiers */
#define INMMBR_KEY      0x0003  /* Member name */
#define INMDIR_KEY      0x000B  /* Directory blocks count */
#define INMEXPDT_KEY    0x0022  /* Expiration date */
#define INMTERM_KEY     0x0028  /* Terminal transmission */
#define INMUTILN_KEY    0x002B  /* Utility name (e.g. INMCOPY, IEBCOPY) */
#define INMSIZE_KEY     0x0030  /* Estimated dataset size */
#define INMDSORG_KEY    0x003C  /* Dataset organization (0x4000=PS, 0x0200=PO) */
#define INMLRECL_KEY    0x0042  /* Logical record length */
#define INMRECFM_KEY    0x0049  /* Record format flags */
#define INMBLKSIZE_KEY  0x0030  /* Block size */

/* DSORG bit flags */
#define DSORG_PS        0x4000  /* Physical Sequential */
#define DSORG_PO        0x0200  /* Partitioned Organization */
#define DSORG_DA        0x2000  /* Direct Access */

/* RECFM bit flags */
#define RECFM_F         0x80    /* Fixed */
#define RECFM_V         0x40    /* Variable */
#define RECFM_U         0xC0    /* Undefined */
#define RECFM_B         0x10    /* Blocked */
#define RECFM_S         0x08    /* Standard (F) or Spanned (V) */
#define RECFM_A         0x04    /* ANSI Control Character */
#define RECFM_M         0x02    /* Machine Control Character */

/* Parsed archive metadata */
typedef struct {
    char        origin_node[16];
    char        origin_user[16];
    char        target_node[16];
    char        target_user[16];
    char        timestamp[24];
    char        dataset_name[64];
    char        member_name[16];
    char        utility_name[16];
    uint16_t    dsorg;
    uint8_t     recfm;
    uint32_t    lrecl;
    uint32_t    blksize;
    uint32_t    dir_blocks;
    uint32_t    num_records;
    bool        has_inmr01;
    bool        has_inmr02;
} netdata_info_t;

/* Netdata Stream Writer (XMIT Encoder) */
typedef struct {
    FILE       *out_fp;
    uint8_t     card_buf[80];
    size_t      card_pos;
    size_t      total_records_written;
    size_t      total_bytes_written;
} netdata_writer_t;

void netdata_writer_init(netdata_writer_t *w, FILE *out_fp);
int  netdata_writer_put_segment(netdata_writer_t *w, uint8_t flags, const uint8_t *data, size_t len);
int  netdata_writer_emit_inmr01(netdata_writer_t *w, const char *node, const char *user);
int  netdata_writer_emit_inmr02(netdata_writer_t *w, const char *dsn, uint16_t dsorg, uint8_t recfm, uint32_t lrecl, uint32_t blksize);
int  netdata_writer_emit_inmr03(netdata_writer_t *w, uint16_t dsorg, uint8_t recfm, uint32_t lrecl);
int  netdata_writer_emit_data(netdata_writer_t *w, const uint8_t *data, size_t len);
int  netdata_writer_emit_inmr06(netdata_writer_t *w);
int  netdata_writer_flush(netdata_writer_t *w);

/* Parse Netdata header from buffer */
int  netdata_parse_header(const uint8_t *buffer, size_t buflen, netdata_info_t *info);

/* Format DSORG and RECFM into human-readable strings */
void netdata_format_dsorg(uint16_t dsorg, char *out, size_t out_sz);
void netdata_format_recfm(uint8_t recfm, char *out, size_t out_sz);

#endif /* NETDATA_H */
