#ifndef DYNALLOC_H
#define DYNALLOC_H

#include <stddef.h>

/* Execute generic dynamic allocation command (BPXWDYN syntax) */
int dynalloc(const char *cmd,
             char *msg,   size_t msglen,
             char *ddn,   size_t ddn_sz,
             char *dsn,   size_t dsn_sz,
             char *vol,   size_t vol_sz);

int dynalloc_str(const char *cmd, char *msg, size_t msglen);

/* High-level allocation helpers */
int dynalloc_temp_dsn(char *out_dsn, size_t out_len, const char *prefix);

int dynalloc_alloc_temp_seq(const char *dsn, int pri_tracks, int sec_tracks);

int dynalloc_free_dsn(const char *dsn);

int dynalloc_delete_dsn(const char *dsn);

#endif /* DYNALLOC_H */
