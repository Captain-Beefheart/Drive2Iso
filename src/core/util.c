#include "drive2iso/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#ifdef _WIN32
#  include <direct.h>
#  define MKDIR(p) _mkdir(p)
#  define POPEN    _popen
#  define PCLOSE   _pclose
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  define MKDIR(p) mkdir((p), 0755)
#  define POPEN    popen
#  define PCLOSE   pclose
#endif

static void vlog(FILE *f, const char *lvl, const char *fmt, va_list ap) {
    fprintf(f, "[%s] ", lvl);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
}
void log_info(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vlog(stdout, "info", fmt, ap); va_end(ap); }
void log_warn(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vlog(stderr, "warn", fmt, ap); va_end(ap); }
void log_err (const char *fmt, ...) { va_list ap; va_start(ap, fmt); vlog(stderr, "err ", fmt, ap); va_end(ap); }

char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char *str_format(const char *fmt, ...) {
    va_list ap;  va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (buf) vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return buf;
}

char *str_slugify(const char *s) {
    if (!s) return NULL;
    char *out = malloc(strlen(s) + 1);
    if (!out) return NULL;
    size_t j = 0;
    int prev_dash = 0;
    for (size_t i = 0; s[i]; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c)) { out[j++] = (char)tolower(c); prev_dash = 0; }
        else if (!prev_dash && j > 0) { out[j++] = '-'; prev_dash = 1; }
    }
    while (j > 0 && out[j - 1] == '-') j--; /* trim trailing dash */
    out[j] = '\0';
    return out;
}

static const char *basename_ptr(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}
char *path_basename(const char *path) { return xstrdup(basename_ptr(path)); }
char *path_basename_noext(const char *path) {
    char *b = xstrdup(basename_ptr(path));
    if (b) { char *dot = strrchr(b, '.'); if (dot && dot != b) *dot = '\0'; }
    return b;
}

char *path_parent(const char *path) {
    const char *last = NULL;
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\') last = p;
    if (!last) return xstrdup(".");
    if (last == path) return xstrdup("/");
    size_t n = (size_t)(last - path);
    char *r = malloc(n + 1);
    if (r) { memcpy(r, path, n); r[n] = '\0'; }
    return r;
}

int ensure_dir(const char *path) {
    MKDIR(path); /* best-effort: ignore EEXIST and the like in the scaffold */
    return 0;
}

int write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) { log_err("cannot write %s", path); return -1; }
    size_t n = strlen(content);
    size_t w = fwrite(content, 1, n, f);
    fclose(f);
    return (w == n) ? 0 : -1;
}

int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) { log_err("cannot open %s", src); return -1; }
    FILE *out = fopen(dst, "wb");
    if (!out) { log_err("cannot create %s", dst); fclose(in); return -1; }
    char buf[1 << 16];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { rc = -1; break; }
    }
    fclose(in);
    fclose(out);
    return rc;
}

bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

char *run_capture(const char *cmd) {
    FILE *fp = POPEN(cmd, "r");
    if (!fp) return NULL;
    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) { PCLOSE(fp); return NULL; }
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, fp)) > 0) {
        if (len + n + 1 > cap) {
            while (len + n + 1 > cap) cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); PCLOSE(fp); return NULL; }
            buf = nb;
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    buf[len] = '\0';
    PCLOSE(fp);
    return buf;
}
