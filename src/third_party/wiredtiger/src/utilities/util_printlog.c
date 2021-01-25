/*-
 * Copyright (c) 2014-2020 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int
usage(void)
{
    static const char *options[] = {"-f", "output to the specified file", "-x",
      "display key and value items in hexadecimal format", "-l",
      "the start LSN from which the log will be printed, optionally the end LSN can also be "
      "specified",
      NULL, NULL};

    util_usage(
      "printlog [-x] [-f output-file] [-l start-file,start-offset]|[-l "
      "start-file,start-offset,end-file,end-offset]",
      "options:", options);
    return (1);
}

int
util_printlog(WT_SESSION *session, int argc, char *argv[])
{
    WT_DECL_RET;
    WT_LSN end_lsn, start_lsn;
    uint32_t end_lsnfile, end_lsnoffset, start_lsnfile, start_lsnoffset;
    uint32_t flags;
    int ch;
    int n_args;
    char *ofile, *start_str;
    bool end_set, start_set;

    end_set = start_set = false;
    flags = 0;
    ofile = NULL;

    while ((ch = __wt_getopt(progname, argc, argv, "f:l:mx")) != EOF)
        switch (ch) {
        case 'f': /* output file */
            ofile = __wt_optarg;
            break;
        case 'l':
            start_str = __wt_optarg;
            n_args = sscanf(start_str, "%" SCNu32 ",%" SCNu32 ",%" SCNu32 ",%" SCNu32,
              &start_lsnfile, &start_lsnoffset, &end_lsnfile, &end_lsnoffset);
            if (n_args == 2) {
                WT_SET_LSN(&start_lsn, start_lsnfile, start_lsnoffset);
                start_set = true;
            } else if (n_args == 4) {
                WT_SET_LSN(&start_lsn, start_lsnfile, start_lsnoffset);
                WT_SET_LSN(&end_lsn, end_lsnfile, end_lsnoffset);
                end_set = start_set = true;
            } else
                return (usage());
            break;
        case 'm': /* messages only */
            LF_SET(WT_TXN_PRINTLOG_MSG);
            break;
        case 'x': /* hex output */
            LF_SET(WT_TXN_PRINTLOG_HEX);
            break;
        case '?':
        default:
            return (usage());
        }
    argc -= __wt_optind;

    /* There should not be any more arguments. */
    if (argc != 0)
        return (usage());

    if ((ret = __wt_txn_printlog(session, ofile, flags, start_set == true ? &start_lsn : NULL,
           end_set == true ? &end_lsn : NULL)) != 0)
        (void)util_err(session, ret, "printlog");

    return (ret);
}
