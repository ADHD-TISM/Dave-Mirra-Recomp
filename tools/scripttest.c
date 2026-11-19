/* Checks runtime/script_parse.h, the parser behind --script and --script-file: events, white space around them (a file's final
   newline, CRLF, spaces after a comma) and unknown button names. From the repository root:
     mkdir -p build && gcc -I runtime tools/scripttest.c -o build/scripttest && build/scripttest */
#include <stdio.h>
#include "script_parse.h"

static int fails;

static void run(const char *what, const char *text, int n, const unsigned *frames, const unsigned *bits) {
    script_n = 0;
    host_set_script(text);
    int ok = script_n == n;
    for (int i = 0; ok && i < n; i++) ok = script[i].frame == frames[i] && script[i].mask == (unsigned short)(1u << bits[i]);
    printf("%-42s %s (%d events, want %d)\n", what, ok ? "ok" : "FAIL", script_n, n);
    if (!ok) fails++;
}

int main(void) {
    static const unsigned f2[] = { 90, 300 }, b2[] = { 3, 14 };            /* st, x */
    static const unsigned f3[] = { 90, 300, 400 }, b3[] = { 3, 14, 13 };   /* st, x, o */
    static const unsigned f1[] = { 6 }, b1[] = { 14 };
    run("two events", "90:st,300:x", 2, f2, b2);
    run("final newline (the old quirk)", "90:st,300:x\n", 2, f2, b2);
    run("final CRLF", "90:st,300:x\r\n", 2, f2, b2);
    run("comma, then newline at the end", "90:st,300:x,\n", 2, f2, b2);
    run("spaces and a trailing comma", "90:st, 300:x ,400:o,\n", 3, f3, b3);
    run("events on separate lines", "  \n90:st,\n300:x\n", 2, f2, b2);
    run("space after the colon", "90: st,300: x", 2, f2, b2);
    run("unknown names are skipped", "5:zz,6:x", 1, f1, b1);
    run("empty text", "", 0, 0, 0);
    run("a lone newline", "\n", 0, 0, 0);
    run("only commas and spaces", " , ,\n", 0, 0, 0);
    printf(fails ? "FAILURES: %d\n" : "all ok\n", fails);
    return fails != 0;
}
