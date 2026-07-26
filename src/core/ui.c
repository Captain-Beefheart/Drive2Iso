#include "drive2iso/ui.h"
#include <stdio.h>

#ifdef _WIN32
#  include <windows.h>
#  ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#    define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#  endif
#endif

/* Etcher-ish teal accent. */
#define TEAL "\x1b[38;2;42;209;209m"
#define DIM  "\x1b[2m"
#define BOLD "\x1b[1m"
#define RST  "\x1b[0m"

void ui_init(void) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleOutputCP(CP_UTF8);
#endif
}

void ui_banner(void) {
    printf("\n  " BOLD TEAL "Drive2Iso" RST
           "  " DIM "— capture a partition into a bootable ISO" RST "\n\n");
    printf("  " TEAL "①" RST " SOURCE   " DIM "›" RST "   "
              TEAL "②" RST " CAPTURE  " DIM "›" RST "   "
              TEAL "③" RST " IMAGE\n\n");
}

void ui_step(int n, int active, const char *label, const char *detail) {
    static const char *num[] = { "•", "①", "②", "③" };
    const char *color = active ? TEAL : DIM;
    const char *glyph = (n >= 1 && n <= 3) ? num[n] : num[0];
    printf("  %s%s %s%s  " DIM "▸" RST "  %s\n",
           color, glyph, label, RST, detail ? detail : "");
}

void ui_progress(const char *label, unsigned long long done, unsigned long long total) {
    const int width = 24;
    double frac = total ? (double)done / (double)total : 0.0;
    if (frac > 1.0) frac = 1.0;
    int fill = (int)(frac * width);
    fprintf(stderr, "\r  " TEAL "%s" RST " [", label ? label : "work");
    for (int i = 0; i < width; ++i) fputc(i < fill ? '#' : '-', stderr);
    fprintf(stderr, "] %3d%%  %llu/%llu MiB",
            (int)(frac * 100), done / (1024ull * 1024ull), total / (1024ull * 1024ull));
    fflush(stderr);
}
