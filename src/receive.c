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

    for (int t = 0; t < timeout_secs; t++) {
        char status_cmd[128];
        snprintf(status_cmd, sizeof(status_cmd), "tsocmd \"STATUS\" 2>&1");
        FILE *p = popen(status_cmd, "r");
        if (p) {
            char buf[8192];
            size_t n = fread(buf, 1, sizeof(buf) - 1, p);
            pclose(p);
            buf[n] = '\0';
            e2a_mem(buf, n);

            if (strstr(buf, jobid)) {
                if (strstr(buf, "OUTPUT QUEUE") || strstr(buf, "output queue")) {
                    log_verbose("Job %s completed successfully", jobid);
                    return 0;
                }
            }
        }
        sleep(1);
    }
    log_error("Timeout waiting for job %s to complete", jobid);
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

    /* Format job name based on current user HLQ so STATUS finds it */
    char jobname[16];
    const char *hlq = get_current_hlq();
    snprintf(jobname, sizeof(jobname), "%.7sR", hlq);

    /* Write temporary JCL file in EBCDIC */
    char jcl_path[128];
    snprintf(jcl_path, sizeof(jcl_path), "/tmp/rcv_%d_%lx.jcl", (int)getpid(), (unsigned long)time(NULL));

    char jcl[1024];
    snprintf(jcl, sizeof(jcl),
             "//%s JOB (1),MSGCLASS=A,CLASS=A\n"
             "//STEP1 EXEC PGM=IKJEFT01\n"
             "//SYSTSPRT DD SYSOUT=*\n"
             "//SYSTSIN DD *\n"
             "  RECEIVE INDSNAME('%s')\n"
             "  RESTORE DATASET('%s')\n"
             "/*\n",
             jobname, temp_dsn, target_dsn);

    size_t jcl_len = strlen(jcl);
    a2e_mem(jcl, jcl_len);

    FILE *jcl_fp = fopen(jcl_path, "wb");
    if (!jcl_fp) {
        log_error("Failed to create temporary JCL file: %s", jcl_path);
        dynalloc_delete_dsn(temp_dsn);
        return RC_ERR_INTERNAL;
    }
    fwrite(jcl, 1, jcl_len, jcl_fp);
    fclose(jcl_fp);

    /* Submit JCL and capture Job ID */
    char submit_cmd[256];
    snprintf(submit_cmd, sizeof(submit_cmd), "/bin/submit -jq %s 2>&1", jcl_path);

    log_verbose("Submitting restore batch job: %s", submit_cmd);
    FILE *p = popen(submit_cmd, "r");
    char jobid[64] = {0};
    if (p) {
        if (fgets(jobid, sizeof(jobid), p)) {
            /* Decode EBCDIC from native submit command */
            e2a_str(jobid);
            size_t jlen = strlen(jobid);
            while (jlen > 0 && (jobid[jlen - 1] == '\n' || jobid[jlen - 1] == '\r' || jobid[jlen - 1] == ' ')) {
                jobid[--jlen] = '\0';
            }
        }
        pclose(p);
    }
    unlink(jcl_path);

    if (jobid[0] == '\0' || strstr(jobid, "JOB") == NULL) {
        log_error("Failed to submit restore job (output: %s)", jobid);
        dynalloc_delete_dsn(temp_dsn);
        return RC_ERR_TSO;
    }

    log_verbose("Submitted job: %s", jobid);

    /* Wait for batch job to finish */
    if (wait_for_job(jobid, 30) != 0) {
        log_error("Restore job %s did not complete within timeout", jobid);
        dynalloc_delete_dsn(temp_dsn);
        return RC_ERR_TSO;
    }

    /* Clean up temporary staging dataset */
    dynalloc_delete_dsn(temp_dsn);

    log_info("Successfully restored dataset %s", target_dsn);
    return RC_SUCCESS;
}
