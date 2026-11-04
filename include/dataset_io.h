#ifndef DATASET_IO_H
#define DATASET_IO_H

#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>

#define XMIT_RECORD_LEN 80

/* Write standard 80-byte records from an MVS sequential dataset to a FILE stream */
int dataset_to_stream(const char *dsn, FILE *out_fp, size_t *records_written);

/* Read 80-byte records from a FILE stream and write to an MVS sequential dataset */
int stream_to_dataset(FILE *in_fp, const char *dsn, size_t *records_read, char *header_buf, size_t header_buf_sz);

#endif /* DATASET_IO_H */
