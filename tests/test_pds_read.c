#define _EXT 1
#define _XOPEN_SOURCE_EXTENDED 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "dynalloc.h"
#include "utils.h"

int main(int argc, char **argv) {
    const char *dsn = (argc > 1) ? argv[1] : "CCW.GO.PDSE";
    const char *mbr = (argc > 2) ? argv[2] : "PASSWD";

    char alloc_cmd[256];
    char msg[256];
    snprintf(alloc_cmd, sizeof(alloc_cmd), "alloc fi(MBRDD) da('%s(%s)') shr", dsn, mbr);
    int rc = dynalloc_str(alloc_cmd, msg, sizeof(msg));
    if (rc != 0) {
        printf("Alloc failed: %s\n", msg);
        return 1;
    }

    FILE *fp = fopen("DD:MBRDD", "rb,type=record");
    if (!fp) {
        printf("fopen failed\n");
        return 2;
    }

    uint8_t buf[32768];
    size_t n;
    size_t total_bytes = 0;
    int blocks = 0;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        blocks++;
        total_bytes += n;
        if (blocks <= 3) {
            printf("Block #%d: len=%zu, first 16 bytes: ", blocks, n);
            for (size_t i = 0; i < n && i < 16; i++) printf("%02X ", buf[i]);
            printf("\n");
        }
    }
    printf("Member '%s': total %d blocks, %zu bytes\n", mbr, blocks, total_bytes);
    fclose(fp);
    dynalloc_str("free fi(MBRDD)", msg, sizeof(msg));
    return 0;
}
