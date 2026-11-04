#define _EXT 1
#define _XOPEN_SOURCE_EXTENDED 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ussxmit.h"
#include "netdata.h"
#include "utils.h"

#include "dynalloc.h"

int do_inspect(const ussxmit_opts_t *opts)
{
    char header_buf[8192];
    size_t nread = 0;
    FILE *fp = NULL;
    char msg[256];
    bool is_dd = false;

    if (opts && opts->source_dsn[0] != '\0') {
        char src_dsn[64];
        dsn_normalize(src_dsn, sizeof(src_dsn), opts->source_dsn);
        char alloc_cmd[256];
        snprintf(alloc_cmd, sizeof(alloc_cmd), "alloc fi(INSPDD) da('%s') shr", src_dsn);
        int rc = dynalloc_str(alloc_cmd, msg, sizeof(msg));
        if (rc != 0) {
            log_error("Failed to dynamically allocate dataset '%s': %s", src_dsn, msg);
            return RC_ERR_ALLOC_SRC;
        }
        fp = fopen("DD:INSPDD", "rb,type=record");
        if (!fp) {
            log_error("Failed to open DD:INSPDD for dataset '%s'", src_dsn);
            dynalloc_str("free fi(INSPDD)", msg, sizeof(msg));
            return RC_ERR_ALLOC_SRC;
        }
        is_dd = true;
    } else {
        fp = stdin;
    }

    /* Read up to 8192 bytes (or 100 80-byte cards) */
    while (nread + 80 <= sizeof(header_buf)) {
        size_t n = fread(header_buf + nread, 1, is_dd ? 80 : sizeof(header_buf) - nread, fp);
        if (n == 0) break;
        nread += n;
        if (nread >= 4096) break;
    }

    if (is_dd) {
        fclose(fp);
        dynalloc_str("free fi(INSPDD)", msg, sizeof(msg));
    }

    log_verbose("do_inspect: read %zu bytes from input source", nread);

    if (nread < 80) {
        log_error("Input source contains fewer than 80 bytes");
        return RC_ERR_STREAM;
    }

    netdata_info_t info;
    if (netdata_parse_header((const uint8_t *)header_buf, nread, &info) != 0) {
        log_error("Failed to parse Netdata INMR01/INMR02 headers from input source (has_inmr01=%d, has_inmr02=%d)",
                  info.has_inmr01, info.has_inmr02);
        return RC_ERR_STREAM;
    }

    char dsorg_str[32];
    char recfm_str[16];
    netdata_format_dsorg(info.dsorg, dsorg_str, sizeof(dsorg_str));
    netdata_format_recfm(info.recfm, recfm_str, sizeof(recfm_str));

    printf("========================================\n");
    printf(" MVS TSO XMIT Archive Information\n");
    printf("========================================\n");
    printf(" Dataset Name : %s\n", info.dataset_name[0] ? info.dataset_name : "(none)");
    if (info.member_name[0]) {
        printf(" Member Name  : %s\n", info.member_name);
    }
    printf(" Organization : %s\n", dsorg_str);
    printf(" Record Format: %s\n", recfm_str);
    printf(" Record Length: %u\n", info.lrecl);
    printf(" Block Size   : %u\n", info.blksize);
    if (info.dir_blocks > 0) {
        printf(" Directory Blk: %u\n", info.dir_blocks);
    }
    if (info.utility_name[0]) {
        printf(" Utility Name : %s\n", info.utility_name);
    }
    if (info.origin_node[0] || info.origin_user[0]) {
        printf(" Origin       : %s.%s\n", info.origin_node, info.origin_user);
    }
    if (info.timestamp[0]) {
        printf(" Timestamp    : %s\n", info.timestamp);
    }
    printf("========================================\n");

    return RC_SUCCESS;
}
