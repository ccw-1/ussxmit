#define _EXT 1
#define _XOPEN_SOURCE_EXTENDED 1
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "netdata.h"
#include "dynalloc.h"

static int record_cb(bool is_control, const uint8_t *data, size_t len, void *user_data) {
    int *count = (int *)user_data;
    (*count)++;
    printf("\n=== Record #%d: is_ctl=%d, len=%zu ===\n", *count, is_control, len);
    for (size_t i = 0; i < len; i += 16) {
        printf("%04zx: ", i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < len) printf("%02X ", data[i+j]);
            else printf("   ");
        }
        printf(" |");
        for (size_t j = 0; j < 16; j++) {
            if (i + j < len) {
                uint8_t c = data[i+j];
                printf("%c", (c >= 32 && c <= 126) ? (char)c : '.');
            }
        }
        printf("|\n");
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *dsn = (argc > 1) ? argv[1] : "CCW.TESTPDS.XMI";
    char alloc_cmd[256];
    char msg[256];
    snprintf(alloc_cmd, sizeof(alloc_cmd), "alloc fi(XMIIN) da('%s') shr", dsn);
    dynalloc_str(alloc_cmd, msg, sizeof(msg));
    FILE *fp = fopen("DD:XMIIN", "rb,type=record");
    if (!fp) {
        printf("fopen failed\n");
        return 1;
    }
    netdata_reader_t reader;
    int count = 0;
    netdata_reader_init(&reader, record_cb, &count);
    uint8_t card[80];
    while (fread(card, 1, 80, fp) == 80) {
        netdata_reader_feed_card(&reader, card);
    }
    fclose(fp);
    dynalloc_str("free fi(XMIIN)", msg, sizeof(msg));
    printf("\nTotal records: %d\n", count);
    return 0;
}
