#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
#include <stdbool.h>

/* Logging configuration and functions */
void log_set_verbose(bool verbose);
void log_set_quiet(bool quiet);

void log_info(const char *fmt, ...);
void log_verbose(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_error(const char *fmt, ...);

/* EBCDIC / ASCII Code page conversion helpers */
void a2e_mem(char *buf, size_t len);
void e2a_mem(char *buf, size_t len);
void a2e_str(char *str);
void e2a_str(char *str);

/* MVS Dataset Name utilities */
void dsn_normalize(char *out_dsn, size_t out_sz, const char *in_dsn);
void dsn_quote(char *out_dsn, size_t out_sz, const char *in_dsn);

/* System user ID helper */
const char *get_current_hlq(void);

#endif /* UTILS_H */
