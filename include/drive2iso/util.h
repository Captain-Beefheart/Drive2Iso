#ifndef D2I_UTIL_H
#define D2I_UTIL_H

#include <stdbool.h>
#include <stddef.h>

/* --- logging --- */
void log_info(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_err (const char *fmt, ...);

/* --- strings (returned buffers are heap-allocated; caller frees) --- */
char *xstrdup(const char *s);
char *str_format(const char *fmt, ...); /* printf into a fresh buffer */
char *str_slugify(const char *s);       /* -> lowercase [a-z0-9-] */

/* --- paths / files --- */
char *path_basename(const char *path);       /* last component, caller frees */
char *path_basename_noext(const char *path); /* last component, extension dropped */
char *path_parent(const char *path);         /* directory part; "." if none */
int   ensure_dir(const char *path);          /* best-effort mkdir (single level) */
int   write_file(const char *path, const char *content);
int   copy_file(const char *src, const char *dst);
bool  file_exists(const char *path);

/* Run a shell command and capture stdout (caller frees). NULL on failure. */
char *run_capture(const char *cmd);

#endif /* D2I_UTIL_H */
