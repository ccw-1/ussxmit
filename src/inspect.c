#define _EXT 1
#define _XOPEN_SOURCE_EXTENDED 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ussxmit.h"
#include "netdata.h"
#include "utils.h"

int do_inspect(const ussxmit_opts_t *opts)
{
    (void)opts;
    char header_buf[4096];
    size_t nread = fread(header_buf, 1, sizeof(header_buf), stdin);

    if (nread < 80) {
        log_error("Input stream contains fewer than 80 bytes");
        return RC_ERR_STREAM;
    }

    netdata_info_t info;
    if (netdata_parse_header((const uint8_t *)header_buf, nread, &info) != 0) {
        log_error("Failed to parse Netdata INMR01/INMR02 headers from input stream");
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
