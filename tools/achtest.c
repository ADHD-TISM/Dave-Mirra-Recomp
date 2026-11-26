/* Check an achievements list (achievements/<game>.json) against RAM dumps (--ramdump files), with the rcheevos
   runtime the game itself uses. Every rule starts on zeroed memory, as at boot, then sees each dump in turn for a few
   frames. Prints the rules that do not parse, and the ones that fire on each dump.
   usage: achtest <list.json> <ram.bin>...      (each dump is the 2 MB main RAM; addresses are PS1 RAM offsets)
   Build (from the project folder; R=third_party/rl_achievements/third_party/rcheevos): compile this file with every
   .c file in $R/src/runtime and $R/src/util and third_party/rl_achievements/third_party/cjson/cJSON.c, with
   -I$R/include -I$R/src -Ithird_party/rl_achievements/third_party/cjson.
   RAM dumps come from the game's --ramdump option, and tools/mkachievements.py writes the lists. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rc_runtime.h"
#include "rc_error.h"
#include "cJSON.h"

#define RAM_SIZE 0x200000
#define MAX_ID 1024
static unsigned char ram[RAM_SIZE], scratch[0x400];   /* main RAM from 0x000000, the scratchpad from 0x200000 */
static int fired[MAX_ID], dump_no;

static uint32_t peek(uint32_t address, uint8_t *buf, uint32_t n, void *ud) {   /* as runtime/achievements.c reads */
    (void)ud;
    if (address < RAM_SIZE && n <= RAM_SIZE - address) { memcpy(buf, ram + address, n); return n; }
    if (address >= RAM_SIZE && address - RAM_SIZE < sizeof scratch && n <= sizeof scratch - (address - RAM_SIZE)) {
        memcpy(buf, scratch + (address - RAM_SIZE), n);   /* the dumps hold no scratchpad: it reads as zero */
        return n;
    }
    return 0;   /* an address the console does not have */
}

static void event(const rc_runtime_event_t *e) {
    if (e->type == RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED && e->id < MAX_ID && !fired[e->id]) fired[e->id] = dump_no;
}

static char *slurp(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)*len + 1);
    if (fread(b, 1, (size_t)*len, f) != (size_t)*len) { fclose(f); free(b); return NULL; }
    fclose(f); b[*len] = 0;
    return b;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: achtest <list.json> <ram.bin>...\n"); return 2; }
    long len; char *text = slurp(argv[1], &len);
    if (!text) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    cJSON *doc = cJSON_Parse(text);
    cJSON *list = doc ? cJSON_GetObjectItem(doc, "achievements") : NULL;
    if (!cJSON_IsArray(list)) { fprintf(stderr, "%s: no achievements array\n", argv[1]); return 2; }
    rc_runtime_t rt; rc_runtime_init(&rt);
    int bad = 0, n = 0;
    cJSON *a;
    cJSON_ArrayForEach(a, list) {
        cJSON *idj = cJSON_GetObjectItemCaseSensitive(a, "id");   /* as the runtime reads the list */
        cJSON *tr = cJSON_GetObjectItemCaseSensitive(a, "trigger");
        if (!cJSON_IsNumber(idj) || !cJSON_IsString(tr)) continue;
        int id = idj->valueint;
        if (id < 0 || id >= MAX_ID) { printf("rule %d: id outside 0..%d, not checked\n", id, MAX_ID - 1); bad++; continue; }
        int r = rc_runtime_activate_achievement(&rt, (uint32_t)id, tr->valuestring, NULL, 0);
        if (r != RC_OK) { printf("rule %d does not parse: %s\n", id, rc_error_str(r)); bad++; }
        n++;
    }
    memset(ram, 0, sizeof ram);
    for (int f = 0; f < 3; f++) rc_runtime_do_frame(&rt, event, peek, NULL, NULL);   /* false first, as at boot */
    for (int i = 2; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f || fread(ram, 1, RAM_SIZE, f) != RAM_SIZE) { fprintf(stderr, "cannot read %s\n", argv[i]); return 2; }
        fclose(f);
        dump_no = i - 1;
        for (int k = 0; k < 3; k++) rc_runtime_do_frame(&rt, event, peek, NULL, NULL);
        printf("%s:", argv[i]);
        for (int id = 0; id < MAX_ID; id++) if (fired[id] == dump_no) printf(" %d", id);
        printf("\n");
    }
    printf("%d rules, %d do not parse\n", n, bad);
    rc_runtime_destroy(&rt);
    cJSON_Delete(doc); free(text);
    return bad ? 1 : 0;
}
