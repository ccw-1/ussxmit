#define _EXT 1
#define _XOPEN_SOURCE_EXTENDED 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ussxmit.h"
#include "dynalloc.h"
#include "dataset_io.h"
#include "utils.h"

int do_xmit(const ussxmit_opts_t *opts)
{
    if (!opts || opts->source_dsn[0] == '\0') {
        log_error("No source dataset name specified for xmit action");
        return RC_ERR_SYNTAX;
    }

    char src_dsn[64];
    dsn_normalize(src_dsn, sizeof(src_dsn), opts->source_dsn);

    char temp_dsn[64];
    if (dynalloc_temp_dsn(temp_dsn, sizeof(temp_dsn), "XMI") != 0) {
        log_error("Failed to generate temporary dataset name");
        return RC_ERR_INTERNAL;
    }

    log_verbose("Source dataset: %s", src_dsn);
    log_verbose("Staging dataset: %s", temp_dsn);

    /* Ensure staging dataset name is clean / unallocated */
    dynalloc_delete_dsn(temp_dsn);

    /* Execute TSO TRANSMIT command (TSO XMIT allocates OUTDSNAME automatically) */
    char tso_cmd[256];
    snprintf(tso_cmd, sizeof(tso_cmd),
             "tsocmd \"XMIT (DUMMY.DUMMY) DATASET('%s') OUTDSNAME('%s') NOLOG\" > /dev/null 2>&1",
             src_dsn, temp_dsn);

    log_verbose("Executing TSO command: %s", tso_cmd);
    int rc = system(tso_cmd);

    if (rc != 0) {
        log_error("TSO TRANSMIT command failed with status %d for dataset '%s'", rc, src_dsn);
        dynalloc_delete_dsn(temp_dsn);
        return RC_ERR_TSO;
    }

    /* Stream records from staging dataset to stdout */
    size_t count = 0;
    if (dataset_to_stream(temp_dsn, stdout, &count) != 0) {
        log_error("Failed to stream dataset records to stdout");
        dynalloc_delete_dsn(temp_dsn);
        return RC_ERR_STREAM;
    }

    /* Clean up temporary staging dataset */
    dynalloc_delete_dsn(temp_dsn);

    log_verbose("Successfully transmitted %zu records from %s", count, src_dsn);
    return RC_SUCCESS;
}
