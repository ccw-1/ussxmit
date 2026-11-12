#define _EXT 1
#define _XOPEN_SOURCE_EXTENDED 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sys/wait.h>

#include "iebcopy.h"
#include "dynalloc.h"
#include "utils.h"

/* IEBCOPY is authorized: it cannot be LINKed from an ordinary USS process (the
 * process is killed), but it runs under the TSO terminal monitor program. So
 * the copy is done by "tsocmd EXEC": a small REXX exec allocates the DDs,
 * CALLs IEBCOPY and frees them. The exec, the control statements and the
 * message data set are temporary cataloged data sets. */

int iebcopy_alloc_unload(const char *dsn, const char *ddname, unsigned long long size_bytes)
{
    char cmd[320];
    char msg[256];
    unsigned long long blocks = size_bytes / IEBCOPY_UNLOAD_BLKSIZE + 1;
    unsigned long long secondary = blocks / 4 + 1;
    if (blocks > 16777215ULL) blocks = 16777215ULL;
    if (secondary > 16777215ULL) secondary = 16777215ULL;
    if (size_bytes == 0) { blocks = 1000; secondary = 5000; }

    snprintf(cmd, sizeof(cmd),
             "alloc fi(%s) da('%s') new catalog block(%u) space(%llu,%llu) release "
             "recfm(v,s) lrecl(%u) blksize(%u)",
             ddname, dsn, IEBCOPY_UNLOAD_BLKSIZE, blocks, secondary,
             IEBCOPY_UNLOAD_LRECL, IEBCOPY_UNLOAD_BLKSIZE);
    log_verbose("dynalloc: %s", cmd);
    int rc = dynalloc_str(cmd, msg, sizeof(msg));
    if (rc != 0) {
        log_error("Failed allocating unload dataset '%s': %s (rc=%d)", dsn, msg, rc);
    }
    return rc;
}

/* Create a cataloged FB (or FBA) data set and fill it with 80-byte text lines
 * converted to EBCDIC; the DD is freed afterwards. lrecl 121 = empty print file. */
static int create_text_dataset(const char *dsn, const char *recfm, unsigned lrecl,
                               const char *const *lines, int nlines)
{
    char cmd[320];
    char msg[256];

    snprintf(cmd, sizeof(cmd),
             "alloc fi(IEBTMP) da('%s') new catalog tracks space(5,5) recfm(%s) lrecl(%u) blksize(%u)",
             dsn, recfm, lrecl, lrecl);
    if (dynalloc_str(cmd, msg, sizeof(msg)) != 0) {
        log_error("Cannot allocate temporary data set '%s': %s", dsn, msg);
        return -1;
    }
    int rc = 0;
    FILE *fp = fopen("DD:IEBTMP", "wb,type=record");
    if (!fp) {
        log_error("Cannot open temporary data set '%s': %s", dsn, strerror(errno));
        rc = -1;
    } else {
        for (int i = 0; i < nlines && rc == 0; i++) {
            char rec[121];
            memset(rec, ' ', lrecl);
            size_t n = strlen(lines[i]);
            if (n > lrecl) n = lrecl;
            memcpy(rec, lines[i], n);
            a2e_mem(rec, lrecl);
            if (fwrite(rec, 1, lrecl, fp) != lrecl) rc = -1;
        }
        fclose(fp);
    }
    dynalloc_str("free fi(IEBTMP)", msg, sizeof(msg));
    return rc;
}

static void delete_dataset(const char *dsn)
{
    dynalloc_delete_dsn(dsn);
}

/* Read the IEBCOPY messages (print data set, ASA control characters) */
static void read_sysprint(const char *dsn, char *log, size_t logsz)
{
    char cmd[320];
    char msg[256];
    if (!log || logsz == 0) return;
    log[0] = '\0';
    snprintf(cmd, sizeof(cmd), "alloc fi(IEBPRT) da('%s') shr", dsn);
    if (dynalloc_str(cmd, msg, sizeof(msg)) != 0) return;
    FILE *fp = fopen("DD:IEBPRT", "rb,type=record");
    if (fp) {
        char rec[256];
        size_t pos = 0, n;
        while ((n = fread(rec, 1, sizeof(rec), fp)) > 0) {
            e2a_mem(rec, n);
            while (n > 0 && (rec[n - 1] == ' ' || rec[n - 1] == '\0')) n--;
            if (n <= 1) continue;               /* blank line (ASA column only) */
            if (pos + n + 1 >= logsz) break;
            memcpy(log + pos, rec + 1, n - 1);  /* drop the ASA control character */
            pos += n - 1;
            log[pos++] = '\n';
            log[pos] = '\0';
        }
        fclose(fp);
    }
    dynalloc_str("free fi(IEBPRT)", msg, sizeof(msg));
}

/* "IEB147I END OF JOB - 00 WAS HIGHEST SEVERITY CODE" -> 0; -1 when absent */
static int highest_severity(const char *log)
{
    const char *p = strstr(log, "WAS HIGHEST SEVERITY CODE");
    if (!p) return -1;
    while (p > log && *(p - 1) == ' ') p--;
    if (p - log < 2) return -1;
    return atoi(p - 2);
}

int iebcopy_copy(const char *in_dsn, const char *out_dsn, char *log, size_t logsz)
{
    char sysin_dsn[64], sysprt_dsn[64], rexx_dsn[64];
    char cmd[400];
    char l[5][90];
    int rc = -1;

    if (log && logsz > 0) log[0] = '\0';

    dynalloc_temp_dsn(sysin_dsn, sizeof(sysin_dsn), "IEBIN");
    dynalloc_temp_dsn(sysprt_dsn, sizeof(sysprt_dsn), "IEBPR");
    dynalloc_temp_dsn(rexx_dsn, sizeof(rexx_dsn), "IEBRX");

    const char *sysin[] = { " COPY OUTDD=SYSUT2,INDD=SYSUT1" };
    if (create_text_dataset(sysin_dsn, "f,b", 80, sysin, 1) != 0) return -1;
    if (create_text_dataset(sysprt_dsn, "f,b,a", 121, NULL, 0) != 0) {
        delete_dataset(sysin_dsn);
        return -1;
    }

    snprintf(l[0], sizeof(l[0]), "\"ALLOC F(SYSUT1) DA('%s') SHR REUSE\"", in_dsn);
    snprintf(l[1], sizeof(l[1]), "\"ALLOC F(SYSUT2) DA('%s') OLD REUSE\"", out_dsn);
    snprintf(l[2], sizeof(l[2]), "\"ALLOC F(SYSIN) DA('%s') SHR REUSE\"", sysin_dsn);
    snprintf(l[3], sizeof(l[3]), "\"ALLOC F(SYSPRINT) DA('%s') OLD REUSE\"", sysprt_dsn);
    snprintf(l[4], sizeof(l[4]), "\"CALL 'SYS1.LINKLIB(IEBCOPY)'\"");
    const char *rexx[] = { "/* REXX */", l[0], l[1], l[2], l[3], l[4],
                           "rcc = rc", "\"FREE F(SYSUT1 SYSUT2 SYSIN SYSPRINT)\"", "exit rcc" };
    if (create_text_dataset(rexx_dsn, "f,b", 80, rexx, 9) != 0) {
        delete_dataset(sysin_dsn);
        delete_dataset(sysprt_dsn);
        return -1;
    }

    snprintf(cmd, sizeof(cmd), "tsocmd \"EXEC '%s' EXEC\" > /dev/null 2>&1", rexx_dsn);
    log_verbose("Running IEBCOPY: COPY OUTDD=SYSUT2,INDD=SYSUT1 ('%s' -> '%s')", in_dsn, out_dsn);
    int st = system(cmd);
    int trc = (st != -1 && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;

    read_sysprint(sysprt_dsn, log, logsz);
    int sev = (log && log[0]) ? highest_severity(log) : -1;
    log_verbose("IEBCOPY: tsocmd rc %d, highest severity code %d", trc, sev);
    rc = (sev >= 0) ? sev : trc;

    delete_dataset(rexx_dsn);
    delete_dataset(sysin_dsn);
    delete_dataset(sysprt_dsn);
    return rc;
}
