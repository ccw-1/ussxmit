#define _EXT 1
#define _XOPEN_SOURCE_EXTENDED 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "ussxmit.h"
#include "utils.h"

static void print_usage(const char *progname)
{
    fprintf(stderr,
        "ussxmit %s — UNIX System Services TSO XMIT & RECEIVE Utility\n"
        "\n"
        "Usage:\n"
        "  %s xmit [options] <source-dataset>\n"
        "  %s receive [options] [target-dataset]\n"
        "  %s inspect [options]\n"
        "\n"
        "Actions:\n"
        "  xmit             Export MVS dataset to standard XMIT stream on STDOUT\n"
        "  receive          Restore XMIT stream from STDIN to MVS dataset\n"
        "  inspect          Display metadata from XMIT stream on STDIN without restoring\n"
        "\n"
        "Options:\n"
        "  -f, --force      Overwrite target dataset if it already exists (receive)\n"
        "  -l, --list       List/inspect metadata only (synonym for inspect)\n"
        "  -v, --verbose    Enable verbose diagnostic messages to STDERR\n"
        "  -q, --quiet      Suppress informational messages\n"
        "  -h, --help       Show this help message and exit\n"
        "  -V, --version    Show version information and exit\n"
        "\n"
        "Examples:\n"
        "  ussxmit xmit \"HLQ.SOURCE.PDS\" > mypds.xmi\n"
        "  ussxmit xmit \"HLQ.PROD.DATA\" | gzip > mydata.xmi.gz\n"
        "  ussxmit receive \"HLQ.RESTORED.PDS\" < mypds.xmi\n"
        "  gunzip -c mydata.xmi.gz | ussxmit receive\n"
        "  ussxmit inspect < mypds.xmi\n",
        USSXMIT_VERSION, progname, progname, progname);
}

static const char *get_base_name(const char *path)
{
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

int main(int argc, char **argv)
{
    ussxmit_opts_t opts;
    memset(&opts, 0, sizeof(opts));

    const char *prog = get_base_name(argv[0]);

    if (strcmp(prog, "xmit") == 0) {
        opts.action = ACTION_XMIT;
    } else if (strcmp(prog, "receive") == 0) {
        opts.action = ACTION_RECEIVE;
    }

    int arg_idx = 1;

    /* Parse action if not determined from executable name */
    if (opts.action == ACTION_UNKNOWN && arg_idx < argc) {
        if (strcmp(argv[arg_idx], "xmit") == 0) {
            opts.action = ACTION_XMIT;
            arg_idx++;
        } else if (strcmp(argv[arg_idx], "receive") == 0) {
            opts.action = ACTION_RECEIVE;
            arg_idx++;
        } else if (strcmp(argv[arg_idx], "inspect") == 0) {
            opts.action = ACTION_INSPECT;
            arg_idx++;
        } else if (strcmp(argv[arg_idx], "-h") == 0 || strcmp(argv[arg_idx], "--help") == 0) {
            print_usage(prog);
            return RC_SUCCESS;
        } else if (strcmp(argv[arg_idx], "-V") == 0 || strcmp(argv[arg_idx], "--version") == 0) {
            printf("ussxmit version %s\n", USSXMIT_VERSION);
            return RC_SUCCESS;
        }
    }

    /* Parse options and dataset arguments */
    while (arg_idx < argc) {
        const char *arg = argv[arg_idx];

        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            opts.verbose = true;
        } else if (strcmp(arg, "-q") == 0 || strcmp(arg, "--quiet") == 0) {
            opts.quiet = true;
        } else if (strcmp(arg, "-f") == 0 || strcmp(arg, "--force") == 0) {
            opts.force = true;
        } else if (strcmp(arg, "-l") == 0 || strcmp(arg, "--list") == 0) {
            opts.action = ACTION_INSPECT;
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(prog);
            return RC_SUCCESS;
        } else if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
            printf("ussxmit version %s\n", USSXMIT_VERSION);
            return RC_SUCCESS;
        } else if (arg[0] == '-') {
            log_error("Unknown option '%s'", arg);
            print_usage(prog);
            return RC_ERR_SYNTAX;
        } else {
            /* Positional dataset name */
            if (opts.action == ACTION_XMIT) {
                if (opts.source_dsn[0] == '\0') {
                    strncpy(opts.source_dsn, arg, sizeof(opts.source_dsn) - 1);
                } else {
                    log_error("Unexpected additional argument '%s'", arg);
                    return RC_ERR_SYNTAX;
                }
            } else if (opts.action == ACTION_RECEIVE) {
                if (opts.target_dsn[0] == '\0') {
                    strncpy(opts.target_dsn, arg, sizeof(opts.target_dsn) - 1);
                } else {
                    log_error("Unexpected additional argument '%s'", arg);
                    return RC_ERR_SYNTAX;
                }
            } else {
                log_error("Unexpected dataset argument for action");
                return RC_ERR_SYNTAX;
            }
        }
        arg_idx++;
    }

    log_set_verbose(opts.verbose);
    log_set_quiet(opts.quiet);

    switch (opts.action) {
        case ACTION_XMIT:
            return do_xmit(&opts);
        case ACTION_RECEIVE:
            return do_receive(&opts);
        case ACTION_INSPECT:
            return do_inspect(&opts);
        default:
            log_error("No action specified (expected 'xmit', 'receive', or 'inspect')");
            print_usage(prog);
            return RC_ERR_SYNTAX;
    }
}
