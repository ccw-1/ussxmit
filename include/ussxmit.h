#ifndef USSXMIT_H
#define USSXMIT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define USSXMIT_VERSION "1.0.0"

/* Return / Exit Codes */
#define RC_SUCCESS          0   /* Success */
#define RC_ERR_SYNTAX       1   /* Command line syntax or argument error */
#define RC_ERR_ALLOC_SRC    2   /* Source dataset error / not found */
#define RC_ERR_ALLOC_TGT    3   /* Target dataset error */
#define RC_ERR_STREAM       4   /* Invalid XMIT stream / header corruption */
#define RC_ERR_TSO          5   /* TSO command execution failure */
#define RC_ERR_INTERNAL     6   /* Internal allocation or memory failure */

typedef enum {
    ACTION_UNKNOWN = 0,
    ACTION_XMIT,
    ACTION_RECEIVE,
    ACTION_INSPECT
} ussxmit_action_t;

typedef struct {
    ussxmit_action_t action;
    char            source_dsn[64];     /* Source dataset name */
    char            target_dsn[64];     /* Target dataset name (optional on receive) */
    char            volume[16];         /* Target volume serial (optional) */
    char            unit[16];           /* Target unit name (optional) */
    bool            force;              /* Overwrite existing target dataset */
    bool            verbose;            /* Verbose diagnostics */
    bool            quiet;              /* Suppress non-error output */
    bool            list_only;          /* Inspect archive metadata only */
} ussxmit_opts_t;

/* Action handler functions */
int do_xmit(const ussxmit_opts_t *opts);
int do_receive(const ussxmit_opts_t *opts);
int do_inspect(const ussxmit_opts_t *opts);

#endif /* USSXMIT_H */
