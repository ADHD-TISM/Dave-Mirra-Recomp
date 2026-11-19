/* The --script / --script-file button script, kept apart from host_common.c so tools/scripttest.c can check it on its own. */
#ifndef SCRIPT_PARSE_H
#define SCRIPT_PARSE_H
#include <stdlib.h>
#include <string.h>

/* --script "frame:btn,frame:btn,..." : press btn for 6 frames starting at frame.
   btn: u d l r x o s t st se l1 r1 l2 r2. White space around the events (a file's final newline, a CRLF, spaces after a comma)
   is ignored; an unknown button name is skipped. */
static struct { unsigned frame; unsigned short mask; } script[2048]; static int script_n;
void host_set_script(const char *t) {
    static const struct { const char *n; int bit; } names[] = {
        {"u",4},{"r",5},{"d",6},{"l",7},{"st",3},{"se",0},{"x",14},{"o",13},{"s",15},{"t",12},
        {"l1",10},{"r1",11},{"l2",8},{"r2",9},{0,0} };
    while (*t && script_n < 2048) {
        unsigned f = (unsigned)strtoul(t, (char **)&t, 10);   /* skips white space before a number */
        if (*t == ':') t++;
        while (*t == ' ' || *t == '\t') t++;
        char nm[8]; int k = 0;
        while (*t && *t != ',' && (unsigned char)*t > ' ' && k < 7) nm[k++] = *t++;
        nm[k] = 0;
        while (*t && *t != ',' && (unsigned char)*t <= ' ') t++;   /* white space after the name, the final newline */
        if (*t == ',') t++;
        for (int i = 0; names[i].n; i++) if (!strcmp(names[i].n, nm)) {
            script[script_n].frame = f; script[script_n].mask = (unsigned short)(1 << names[i].bit); script_n++;
        }
    }
}
#endif
