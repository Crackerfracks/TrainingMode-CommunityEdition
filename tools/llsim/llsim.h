/* llsim.h: what main.c (the PC side) calls in sim.c (the game side). */
#ifndef LLSIM_H
#define LLSIM_H

void llsim_common(int at, const unsigned *w, int n);
void llsim_attr(int at, const unsigned *w, int n);
void llsim_special(int at, const unsigned *w, int n);
void llsim_softland(unsigned bits);
int llsim_group(int g, const int *v);
int llsim_line(int i, const int *v, unsigned xc, unsigned xd, unsigned xe, unsigned flags);
int llsim_vert(int i, unsigned cx, unsigned cy, unsigned ox, unsigned oy);
int llsim_ready(int fighter_kind, int wlground);
void llsim_check(void);
void llsim_table_begin(int keep_lean);
void llsim_nolean(void);
int llsim_ecb(int ts, int frame, int flags, const unsigned *w);
int llsim_len(int ts, int len, int next);
void llsim_loops(const int *loops);
void llsim_step(int step);
int llsim_fall_step(void);
void llsim_blend(int blend);
int llsim_fall_blend(void);
void llsim_ecb_print(int fighter_kind);
void llsim_jumps(float sx, float sy, int gap);
void llsim_lag(void);
int llsim_packed(const unsigned char *file, int size, int fighter_kind);

#endif
