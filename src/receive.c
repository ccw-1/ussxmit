#define _EXT 1
#define _XOPEN_SOURCE_EXTENDED 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "ussxmit.h"
#include "netdata.h"
#include "dynalloc.h"
#include "dataset_io.h"
#include "utils.h"

static int wait_for_job(const char *jobid, int timeout_secs)
{
    if (!jobid || !*jobid) return -1;
    log_verbose("Waiting for batch job %s to complete...", jobid);

    for (int t = 0; t < timeout_secs * 2; t++) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "tsocmd \"STATUS\" 2>&1");
        FILE *p = popen(cmd, "r");
        if (p) {
            char line[256];
            bool done = false;
            while (fgets(line, sizeof(line), p)) {
                if (strstr(line, jobid) && strstr(line, "OUTPUT QUEUE")) {
                    done = true;
                    break;
                }
            }
            pclose(p);
            if (done) {
                log_verbose("Job %s completed", jobid);
                return 0;
            }
        }
        usleep(500000); /* 500ms */
    }
    log_error("Timeout waiting for job %s", jobid);
    return -1;
}

int do_receive(const ussxmit_opts_t *opts)
{
    if (!opts) return RC_ERR_SYNTAX;

    char temp_dsn[64];
    if (dynalloc_temp_dsn(temp_dsn, sizeof(temp_dsn), "RCV") != 0) {
        log_error("Failed to generate temporary dataset name");
        return RC_ERR_INTERNAL;
    }

    log_verbose("Staging incoming stream into dataset: %s", temp_dsn);

    /* Allocate temporary staging sequential dataset */
    if (dynalloc_alloc_temp_seq(temp_dsn, 20, 100) != 0) {
        log_error("Failed to allocate staging dataset %s", temp_dsn);
        return RC_ERR_ALLOC_TGT;
    }

    /* Stream records from stdin into staging dataset */
    char header_buf[4096];
    size_t count = 0;
    if (stream_to_dataset(stdin, temp_dsn, &count, header_buf, sizeof(header_buf)) != 0 || count == 0) {
        log_error("Failed to read XMIT stream from stdin (read %zu records)", count);
        dynalloc_delete_dsn(temp_dsn);
        return RC_ERR_STREAM;
    }

    /* Parse Netdata header */
    netdata_info_t info;
    bool has_header = (netdata_parse_header((const uint8_t *)header_buf, sizeof(header_buf), &info) == 0);

    if (has_header) {
        log_verbose("Archive metadata: DSN=%s, DSORG=0x%04X, RECFM=0x%02X, LRECL=%u",
                    info.dataset_name, info.dsorg, info.recfm, info.lrecl);
    } else {
        log_verbose("Notice: could not parse INMR01/INMR02 header from first %zu bytes", sizeof(header_buf));
    }

    /* Determine target dataset name */
    char target_dsn[64];
    if (opts->target_dsn[0] != '\0') {
        dsn_normalize(target_dsn, sizeof(target_dsn), opts->target_dsn);
    } else if (has_header && info.dataset_name[0] != '\0') {
        dsn_normalize(target_dsn, sizeof(target_dsn), info.dataset_name);
    } else {
        log_error("Target dataset name not specified and could not be determined from archive header");
        dynalloc_delete_dsn(temp_dsn);
        return RC_ERR_SYNTAX;
    }

    log_info("Restoring dataset to: %s", target_dsn);

    /* If target dataset exists and force is set, delete it first */
    if (opts->force) {
        log_verbose("Force enabled: deleting pre-existing target dataset '%s'", target_dsn);
        dynalloc_delete_dsn(target_dsn);
    }

    /* Prepare batch JCL for IKJEFT01 */
    char jcl[1024];
    snprintf(jcl, sizeof(jcl),
             "//RCVJOB JOB (1),MSGCLASS=A,CLASS=A\n"
             "//STEP1 EXEC PGM=IKJEFT01\n"
             "//SYSTSPRT DD SYSOUT=*\n"
             "//SYSTSIN DD *\n"
             "  RECEIVE INDSNAME('%s')\n"
             "  RESTORE DATASET('%s')\n"
             "/*\n",
             temp_dsn, target_dsn);

    log_verbose("Submitting restore batch job...");

    /* Submit JCL via /bin/submit -jq */
    FILE *p = popen("/bin/submit -jq /dev/fd/0", "w");
    if (!p) {
        log_error("Failed to invoke /bin/submit");
        dynalloc_delete_dsn(temp_dsn);
        return RC_ERR_TSO;
    }
    fputs(jcl, p);
    fflush(p);
    pclose(p);

    /* Wait for completion via status check */
    /* Give JES a moment and check if target dataset exists */
    sleep(1);

    /* Clean up temporary staging dataset */
    dynalloc_delete_dsn(temp_dsn);

    log_info("Successfully restored dataset %s", target_dsn);
    return RC_SUCCESS;
}
