#define _EXT 1
#define _XOPEN_SOURCE_EXTENDED 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "ussxmit.h"
#include "netdata.h"
#include "utils.h"

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST_ASSERT(cond, msg) do { \
    g_tests_run++; \
    if (cond) { \
        g_tests_passed++; \
        printf("  [PASS] %s\n", msg); \
    } else { \
        printf("  [FAIL] %s (line %d)\n", msg, __LINE__); \
    } \
} while (0)

static void test_ebcdic_roundtrip(void)
{
    printf("\n--- Test: EBCDIC / ASCII Round-trip ---\n");
    char original[] = "HELLO.MVS.DATASET.NAME.123";
    char buffer[64];
    strncpy(buffer, original, sizeof(buffer));

    a2e_str(buffer);
#if ' ' != 0x40
    /* In ASCII build, buffer should now differ from ASCII original */
    TEST_ASSERT(strcmp(buffer, original) != 0, "a2e_str alters ASCII buffer");
#endif

    e2a_str(buffer);
    TEST_ASSERT(strcmp(buffer, original) == 0, "e2a_str restores original string exactly");
}

static void test_format_helpers(void)
{
    printf("\n--- Test: DSORG and RECFM Formatter ---\n");
    char dsorg_str[32];
    char recfm_str[32];

    netdata_format_dsorg(DSORG_PS, dsorg_str, sizeof(dsorg_str));
    TEST_ASSERT(strstr(dsorg_str, "PS") != NULL, "DSORG_PS formats to PS string");

    netdata_format_dsorg(DSORG_PO, dsorg_str, sizeof(dsorg_str));
    TEST_ASSERT(strstr(dsorg_str, "PO") != NULL, "DSORG_PO formats to PO string");

    netdata_format_recfm(RECFM_F | RECFM_B, recfm_str, sizeof(recfm_str));
    TEST_ASSERT(strcmp(recfm_str, "FB") == 0, "RECFM_F | RECFM_B formats to FB");

    netdata_format_recfm(RECFM_V | RECFM_B, recfm_str, sizeof(recfm_str));
    TEST_ASSERT(strcmp(recfm_str, "VB") == 0, "RECFM_V | RECFM_B formats to VB");

    netdata_format_recfm(RECFM_U, recfm_str, sizeof(recfm_str));
    TEST_ASSERT(strcmp(recfm_str, "U") == 0, "RECFM_U formats to U");
}

static void test_netdata_encode_decode(void)
{
    printf("\n--- Test: Netdata Header Encode and Parse ---\n");

    /* Create temporary file for in-memory stream */
    FILE *tmp = tmpfile();
    TEST_ASSERT(tmp != NULL, "Create temp file for writer stream");

    netdata_writer_t writer;
    netdata_writer_init(&writer, tmp);

    int rc = netdata_writer_emit_inmr01(&writer, "POKNODE", "TESTUSER");
    TEST_ASSERT(rc == 0, "Emit INMR01 transmission header");

    rc = netdata_writer_emit_inmr02(&writer, "MY.TEST.SEQDSN", DSORG_PS, RECFM_F | RECFM_B, 80, 3120);
    TEST_ASSERT(rc == 0, "Emit INMR02 dataset attributes");

    const char test_line[] = "Line 1: Sample data record payload for test verification.";
    rc = netdata_writer_emit_data(&writer, (const uint8_t *)test_line, strlen(test_line));
    TEST_ASSERT(rc == 0, "Emit data record segment");

    rc = netdata_writer_emit_inmr06(&writer);
    TEST_ASSERT(rc == 0, "Emit INMR06 trailer record");

    rc = netdata_writer_flush(&writer);
    TEST_ASSERT(rc == 0, "Flush writer to 80-byte card boundary");

    /* Rewind and read back */
    rewind(tmp);
    uint8_t buffer[4096];
    size_t nread = fread(buffer, 1, sizeof(buffer), tmp);
    fclose(tmp);

    TEST_ASSERT(nread >= 80, "Read back generated Netdata stream");
    TEST_ASSERT((nread % 80) == 0, "Stream length is multiple of 80-byte physical cards");

    netdata_info_t info;
    rc = netdata_parse_header(buffer, nread, &info);
    TEST_ASSERT(rc == 0, "netdata_parse_header succeeds on generated stream");
    TEST_ASSERT(strcmp(info.dataset_name, "MY.TEST.SEQDSN") == 0, "Parsed dataset name matches MY.TEST.SEQDSN");
    TEST_ASSERT(info.dsorg == DSORG_PS, "Parsed DSORG matches DSORG_PS");
    TEST_ASSERT(info.recfm == (RECFM_F | RECFM_B), "Parsed RECFM matches FB");
    TEST_ASSERT(info.lrecl == 80, "Parsed LRECL matches 80");
    TEST_ASSERT(info.blksize == 3120, "Parsed BLKSIZE matches 3120");
    TEST_ASSERT(strcmp(info.origin_node, "POKNODE") == 0, "Parsed origin node matches POKNODE");
    TEST_ASSERT(strcmp(info.origin_user, "TESTUSER") == 0, "Parsed origin user matches TESTUSER");
}

typedef struct {
    size_t count;
    size_t last_len;
    uint8_t last_data[2048];
} reader_test_ctx_t;

static int test_record_cb(bool is_control, const uint8_t *data, size_t len, void *user_data)
{
    reader_test_ctx_t *ctx = (reader_test_ctx_t *)user_data;
    if (!is_control) {
        ctx->count++;
        ctx->last_len = len;
        if (len < sizeof(ctx->last_data)) {
            memcpy(ctx->last_data, data, len);
        }
    }
    return 0;
}

static void test_spanned_records(void)
{
    printf("\n--- Test: Spanned Large Data Records (>253 bytes) ---\n");

    FILE *tmp = tmpfile();
    TEST_ASSERT(tmp != NULL, "Create temp file for spanned record test");

    netdata_writer_t writer;
    netdata_writer_init(&writer, tmp);

    netdata_writer_emit_inmr01(&writer, "POK", "CCW");
    netdata_writer_emit_inmr02(&writer, "SPANNED.DATA", DSORG_PS, RECFM_V | RECFM_B, 1000, 32760);

    /* Generate 600-byte test pattern */
    uint8_t large_payload[600];
    for (size_t i = 0; i < sizeof(large_payload); i++) {
        large_payload[i] = (uint8_t)(i & 0xFF);
    }

    int rc = netdata_writer_emit_data(&writer, large_payload, sizeof(large_payload));
    TEST_ASSERT(rc == 0, "Emit 600-byte spanned data record");

    netdata_writer_emit_inmr06(&writer);
    netdata_writer_flush(&writer);

    /* Read back and feed to netdata_reader */
    rewind(tmp);
    uint8_t buffer[8192];
    size_t nread = fread(buffer, 1, sizeof(buffer), tmp);
    fclose(tmp);

    TEST_ASSERT(nread > 0 && (nread % 80) == 0, "Read back spanned records in 80-byte cards");

    reader_test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    netdata_reader_t reader;
    netdata_reader_init(&reader, test_record_cb, &ctx);
    rc = netdata_reader_feed(&reader, buffer, nread);

    TEST_ASSERT(rc == 0, "Reader feeds stream successfully");
    TEST_ASSERT(ctx.count == 1, "Reader dispatched exactly 1 data record");
    TEST_ASSERT(ctx.last_len == 600, "Dispatched data record length is exactly 600 bytes");
    TEST_ASSERT(memcmp(ctx.last_data, large_payload, 600) == 0, "Payload bytes match original pattern identically");
}

int main(void)
{
    printf("==================================================\n");
    printf(" Running ussxmit Unit Test Suite\n");
    printf("==================================================\n");

    test_ebcdic_roundtrip();
    test_format_helpers();
    test_netdata_encode_decode();
    test_spanned_records();

    printf("\n==================================================\n");
    printf(" Test Results: %d / %d tests passed\n", g_tests_passed, g_tests_run);
    printf("==================================================\n");

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
