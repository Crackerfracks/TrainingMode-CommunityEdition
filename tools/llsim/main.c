/* llsim: Landing Lab's simulation (src/landinglab_sim.h) on a PC.
 *
 * It reads a dump the event wrote to the log (script command "dump":
 * LLDUMP lines with the common fighter table, the fighter's attributes and
 * the stage's collision), loads it where the game keeps it (sim.c), and
 * runs the same code the event runs.
 *
 *   llsim check <dump> [wlground]   the hops Sim_Check predicts in game, as
 *                                   LLCHECK lines to diff against its log
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

int main(int argc, char **argv)
{
    if (argc < 3 || strcmp(argv[1], "check"))
    {
        fprintf(stderr, "usage: llsim check <dump> [wlground]\n");
        return 2;
    }
    Dump_Load(argv[2]);
    if (dump_stage < 0 || !llsim_ready(dump_char, argc > 3 ? atoi(argv[3]) : 1))
    {
        fprintf(stderr, "llsim: %s has no LLDUMP begin or collision\n", argv[2]);
        return 1;
    }
    llsim_check();
    return 0;
}
