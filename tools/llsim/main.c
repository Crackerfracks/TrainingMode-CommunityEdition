/* llsim: Landing Lab's simulation (src/landinglab_sim.h) on a PC.
 *
 * It reads a dump the event wrote to the log (script command "dump":
 * LLDUMP lines with the common fighter table, the fighter's attributes and
 * the stage's collision), loads it where the game keeps it (sim.c), and
 * runs the same code the event runs.
 *
 *   llsim check <dump> [wlground] [-t <capture>]
 *       the hops Sim_Check predicts in game, as LLCHECK lines to diff
 *       against its log
 *   llsim ecb <dump> [-t <capture>]
 *       the ECB table in use, as the game's LLECB and LLLEN lines
 *
 * The ECB table is Falcon's baked one unless -t names a capture's log
 * (src/landinglab_capture.txt), whose LLECB and LLLEN lines replace it.
 * -p names a packed TM/llecb.bin (tools/llsim/ecbpack.py) to load the
 * dump's fighter from, as the event does for every fighter but Falcon.
 * The fall leans in use (Falcon's, or a packed fighter's own) stay, as in
 * game, unless -nolean drops them.
 * -loops A,B,C sets the falls' loops (Fall, FallAerial, FallSpecial), as
 * the event learns them from the animations (its LLFALL lines).
 *
 * Build: tools/llsim/build.sh
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llsim.h"

// what the game side needs from the C library, with MexTK's argument types
void *ll_calloc(int size)
{
    return calloc(1, (size_t)size);
}

void ll_memcpy(void *dest, const void *src, int size)
{
    memcpy(dest, src, (size_t)size);
}

void ll_memset(void *dest, int c, int size)
{
    memset(dest, c, (size_t)size);
}

void OSReport(char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

// "<offset> w w w ...", hex: the words at that offset
static void Load_Words(char *t, void (*put)(int, const unsigned *, int))
{
    unsigned w[64];
    int n = 0, at = (int)strtol(t, &t, 16);
    for (;;)
    {
        char *e;
        unsigned long v = strtoul(t, &e, 16);
        if (e == t || n == 64)
            break;
        w[n++] = (unsigned)v;
        t = e;
    }
    put(at, w, n);
}

static int dump_stage = -1, dump_char = -1;

static void Dump_Load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
    {
        fprintf(stderr, "llsim: can't open %s\n", path);
        exit(1);
    }
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        char *t = strstr(line, "LLDUMP ");
        if (!t)
            continue;
        t += 7;
        char what[16];
        int n = 0;
        if (sscanf(t, "%15s %n", what, &n) < 1)
            continue;
        char *rest = t + n;
        if (!strcmp(what, "begin"))
            sscanf(rest, "stage %d char %d", &dump_stage, &dump_char);
        else if (!strcmp(what, "common"))
            Load_Words(rest, llsim_common);
        else if (!strcmp(what, "attr"))
            Load_Words(rest, llsim_attr);
        else if (!strcmp(what, "special"))
            Load_Words(rest, llsim_special);
        else if (!strcmp(what, "softland"))
            llsim_softland((unsigned)strtoul(rest, 0, 16));
        else if (!strcmp(what, "group"))
        {
            int g, v[13];
            if (sscanf(rest, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d", &g, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
                       &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &v[12]) == 14)
                llsim_group(g, v);
        }
        else if (!strcmp(what, "line"))
        {
            int i, v[6];
            unsigned xc, xd, xe, flags;
            if (sscanf(rest, "%d %d %d %d %d %d %d %x %x %x %x", &i, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &xc, &xd, &xe,
                       &flags) == 11)
                llsim_line(i, v, xc, xd, xe, flags);
        }
        else if (!strcmp(what, "vert"))
        {
            int i;
            unsigned cx, cy, ox, oy;
            if (sscanf(rest, "%d %x %x %x %x", &i, &cx, &cy, &ox, &oy) == 5)
                llsim_vert(i, cx, cy, ox, oy);
        }
    }
    fclose(f);
}

// A capture's LLECB and LLLEN lines in place of the baked table.
static void Table_Load(const char *path, int keep_lean)
{
    FILE *f = fopen(path, "r");
    if (!f)
    {
        fprintf(stderr, "llsim: can't open %s\n", path);
        exit(1);
    }
    char line[512];
    int table_char = -1, rows = 0, lens = 0;
    while (fgets(line, sizeof(line), f))
    {
        char *t;
        int ts, frame, flags, len, next;
        unsigned w[5];
        if ((t = strstr(line, "LLECB begin char ")))
        {
            table_char = atoi(t + 17);
            llsim_table_begin(keep_lean);
        }
        else if (table_char < 0)
            continue;
        else if ((t = strstr(line, "LLECB ")) &&
                 sscanf(t + 6, "%d %d %d %x %x %x %x %x", &ts, &frame, &flags, &w[0], &w[1], &w[2], &w[3], &w[4]) == 8)
            rows += llsim_ecb(ts, frame, flags, w);
        else if ((t = strstr(line, "LLLEN ")) && sscanf(t + 6, "%d %d %d", &ts, &len, &next) == 3)
            lens += llsim_len(ts, len, next);
        else if ((t = strstr(line, "LLLOOP ")))
        {
            int loops[3];
            if (sscanf(t + 7, "%d %d %d", &loops[0], &loops[1], &loops[2]) == 3)
                llsim_loops(loops);
        }
    }
    fclose(f);
    if (table_char < 0 || rows == 0)
    {
        fprintf(stderr, "llsim: %s has no LLECB table\n", path);
        exit(1);
    }
    if (table_char != dump_char)
        fprintf(stderr, "llsim: warning: %s is char %d's table, the dump is char %d's\n", path, table_char, dump_char);
}

static void Packed_Load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        fprintf(stderr, "llsim: can't open %s\n", path);
        exit(1);
    }
    static unsigned char file[1 << 20];
    int size = (int)fread(file, 1, sizeof(file), f);
    fclose(f);
    int rows = llsim_packed(file, size, dump_char);
    if (rows < 0)
    {
        fprintf(stderr, "llsim: %s has no table for char %d\n", path, dump_char);
        exit(1);
    }
    fprintf(stderr, "llsim: char %d: %d rows from %s\n", dump_char, rows, path);
}

int main(int argc, char **argv)
{
    const char *table = 0, *packed = 0;
    char *pos[4];
    int npos = 0, keep_lean = 1, loops[3] = {0, 0, 0}, set_loops = 0;
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-t") && i + 1 < argc)
            table = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc)
            packed = argv[++i];
        else if (!strcmp(argv[i], "-loops") && i + 1 < argc)
            set_loops = sscanf(argv[++i], "%d,%d,%d", &loops[0], &loops[1], &loops[2]) == 3;
        else if (!strcmp(argv[i], "-nolean"))
            keep_lean = 0;
        else if (npos < 4)
            pos[npos++] = argv[i];
    }
    int check = npos >= 2 && !strcmp(pos[0], "check"), ecb = npos == 2 && !strcmp(pos[0], "ecb");
    if (!check && !ecb)
    {
        fprintf(stderr, "usage: llsim check <dump> [wlground] [-t <capture>] [-p <llecb.bin>] [-loops A,B,C] [-nolean]\n"
                        "       llsim ecb <dump> [-t <capture>] [-p <llecb.bin>]\n");
        return 2;
    }
    Dump_Load(pos[1]);
    if (dump_stage < 0 || !llsim_ready(dump_char, npos > 2 ? atoi(pos[2]) : 1))
    {
        fprintf(stderr, "llsim: %s has no LLDUMP begin or collision\n", pos[1]);
        return 1;
    }
    if (packed)
        Packed_Load(packed);
    if (table)
        Table_Load(table, keep_lean);
    if (set_loops)
        llsim_loops(loops);
    if (!keep_lean)
        llsim_nolean();
    if (ecb)
        llsim_ecb_print(dump_char);
    else
        llsim_check();
    return 0;
}
