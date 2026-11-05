#define _EXT 1
#define _XOPEN_SOURCE_EXTENDED 1
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "dynalloc.h"

int main() {
    char msg[256];
    dynalloc_delete_dsn("CCW.TESTPDS.NEW");

    /* Allocate new PDS */
    int rc = dynalloc_str("alloc fi(NEWPDS) da('CCW.TESTPDS.NEW') new catalog dir(10) tracks space(1,5) recfm(f,b) lrecl(80) blksize(3120)", msg, sizeof(msg));
    printf("Alloc PDS rc=%d, msg=%s\n", rc, msg);
    dynalloc_str("free fi(NEWPDS)", msg, sizeof(msg));

    /* Allocate member 1 */
    rc = dynalloc_str("alloc fi(MBR1) da('CCW.TESTPDS.NEW(MEM1)') shr", msg, sizeof(msg));
    printf("Alloc MBR1 rc=%d, msg=%s\n", rc, msg);
    FILE *fp = fopen("DD:MBR1", "wb,type=record");
    if (!fp) {
        printf("fopen MBR1 failed\n");
        return 1;
    }
    char rec[80];
    memset(rec, 0x40, sizeof(rec));
    memcpy(rec, "HELLO FROM USSNXMIT MEMBER 1", 28);
    fwrite(rec, 1, 80, fp);
    fclose(fp);
    dynalloc_str("free fi(MBR1)", msg, sizeof(msg));

    /* Allocate member 2 */
    rc = dynalloc_str("alloc fi(MBR2) da('CCW.TESTPDS.NEW(MEM2)') shr", msg, sizeof(msg));
    printf("Alloc MBR2 rc=%d, msg=%s\n", rc, msg);
    fp = fopen("DD:MBR2", "wb,type=record");
    if (!fp) {
        printf("fopen MBR2 failed\n");
        return 1;
    }
    memset(rec, 0x40, sizeof(rec));
    memcpy(rec, "HELLO FROM USSNXMIT MEMBER 2", 28);
    fwrite(rec, 1, 80, fp);
    fclose(fp);
    dynalloc_str("free fi(MBR2)", msg, sizeof(msg));

    /* Now read directory to verify members exist */
    rc = dynalloc_str("alloc fi(READDIR) da('CCW.TESTPDS.NEW') shr", msg, sizeof(msg));
    fp = fopen("DD:READDIR", "rb,type=record");
    uint8_t dirblk[256];
    size_t n = fread(dirblk, 1, sizeof(dirblk), fp);
    printf("Read dir block: n=%zu\n", n);
    if (n >= 2) {
        uint16_t used = (dirblk[0] << 8) | dirblk[1];
        printf("Dir block used bytes: %u\n", used);
        size_t off = 2;
        while (off + 12 <= used) {
            if (dirblk[off] == 0xFF) break;
            printf("Found member in directory: raw hex=%02X%02X%02X%02X%02X%02X%02X%02X\n",
                   dirblk[off], dirblk[off+1], dirblk[off+2], dirblk[off+3],
                   dirblk[off+4], dirblk[off+5], dirblk[off+6], dirblk[off+7]);
            uint8_t c = dirblk[off + 11] & 0x1F;
            off += 12 + c * 2;
        }
    }
    fclose(fp);
    dynalloc_str("free fi(READDIR)", msg, sizeof(msg));
    return 0;
}
