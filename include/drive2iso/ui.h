#ifndef D2I_UI_H
#define D2I_UI_H

/* A light, Balena-Etcher-flavored CLI presentation: a teal banner and the
 * signature three-step strip (SOURCE -> CAPTURE -> IMAGE). */

void ui_init(void);   /* enable ANSI colors / UTF-8 on Windows consoles */
void ui_banner(void); /* app banner + the three-step strip */

/* Print one step line. n is 1..3; active picks teal vs dim. */
void ui_step(int n, int active, const char *label, const char *detail);

/* Redraw an in-place progress bar on stderr (no trailing newline). */
void ui_progress(const char *label, unsigned long long done, unsigned long long total);

#endif /* D2I_UI_H */
