#!/usr/bin/env python3
"""Build llsim dumps from the game's own files instead of a Dolphin run.

Stage collision comes from a parsed stage file (the JSON Codex's
stage_assets.py writes: initialized_geometry per Gr*.dat). Fighter attributes
come straight from Pl*.dat (ftData root: +0 attributes, +4 special
attributes) and the common table from PlCo.dat.

    assets.py <stages.json> <raw dir> <out dir> [stage ...] [--chars Ca,Fx,...]

writes <out dir>/<stage>_<char>.dump in the LLDUMP format main.c reads.
`assets.py --check <stages.json> <raw dir> <game dump>` rebuilds the game
dump's stage and character and reports every word that differs.
"""
import json
import os
import struct
import sys

# Gr file -> GRKINDEXT_ value (MexTK stage.h), the legal stages first.
STAGES = {
    'GrNBa.dat': ('battlefield', 31), 'GrNLa.dat': ('fd', 32), 'GrOp.dat': ('dreamland', 28),
    'GrSt.dat': ('yoshis', 8), 'GrIz.dat': ('fountain', 2), 'GrPs.dat': ('stadium', 3),
}
# Pl file code -> (name, internal kind)
CHARS = {
    'Mr': ('mario', 0), 'Fx': ('fox', 1), 'Ca': ('falcon', 2), 'Dk': ('dk', 3), 'Kb': ('kirby', 4),
    'Kp': ('bowser', 5), 'Lk': ('link', 6), 'Sk': ('sheik', 7), 'Ns': ('ness', 8), 'Pe': ('peach', 9),
    'Pp': ('popo', 10), 'Nn': ('nana', 11), 'Pk': ('pikachu', 12), 'Ss': ('samus', 13), 'Ys': ('yoshi', 14),
    'Pr': ('puff', 15), 'Mt': ('mewtwo', 16), 'Lg': ('luigi', 17), 'Ms': ('marth', 18), 'Zd': ('zelda', 19),
    'Cl': ('ylink', 20), 'Dr': ('doc', 21), 'Fc': ('falco', 22), 'Pc': ('pichu', 23), 'Gw': ('gaw', 24),
    'Gn': ('ganon', 25), 'Fe': ('roy', 26),
}
ATTR_SIZE = 0x184
SPECIAL_SIZE = 0x60
COMMON_SIZE = 0x480


class Dat:
    def __init__(self, path):
        self.raw = open(path, 'rb').read()
        size, data_size, reloc_n, root_n, ref_n = struct.unpack_from('>5I', self.raw, 0)
        self.data = self.raw[0x20:0x20 + data_size]
        roots = 0x20 + data_size + reloc_n * 4
        strings = roots + (root_n + ref_n) * 8
        self.roots = {}
        for i in range(root_n):
            off, name = struct.unpack_from('>2I', self.raw, roots + i * 8)
            end = self.raw.index(b'\0', strings + name)
            self.roots[self.raw[strings + name:end].decode()] = off

    def word(self, off):
        return struct.unpack_from('>I', self.data, off)[0]

    def words(self, off, size):
        # Reads past the data section (a short special block) come back as 0.
        return [self.word(off + i) if off + i + 4 <= len(self.data) else 0 for i in range(0, size, 4)]


def fighter_words(raw_dir, code):
    dat = Dat(os.path.join(raw_dir, 'Pl%s.dat' % code))
    root = [v for k, v in dat.roots.items() if k.startswith('ftData')]
    if len(root) != 1:
        raise SystemExit('Pl%s.dat: expected one ftData root, found %d' % (code, len(root)))
    attr, special = dat.word(root[0]), dat.word(root[0] + 4)
    return dat.words(attr, ATTR_SIZE), dat.words(special, SPECIAL_SIZE)


def common_words(raw_dir):
    dat = Dat(os.path.join(raw_dir, 'PlCo.dat'))
    return dat.words(dat.word(dat.roots['ftLoadCommonData']), COMMON_SIZE)


def f32(bits):
    return struct.unpack('>f', struct.pack('>I', bits))[0]


def stage_lines(archive):
    geo = archive['components']['initialized_geometry']
    if geo['status'] not in ('supported_static_initialization_subset', 'parsed'):
        raise SystemExit('%s: no initialized geometry (%s)' % (archive['archive'], geo['status']))
    raw = archive['components']['raw_collision']['data']
    out = []
    order = ('floor', 'ceiling', 'right_wall', 'left_wall', 'dynamic')
    for g, (rj, gj) in enumerate(zip(raw['joints'], geo['joints'])):
        r = rj['ranges']
        v = [x for k in order for x in (r[k]['start'], r[k]['count'])]
        v += [rj['vertex_start'], rj['vertex_count'], 1 if gj['runtime_flags'] & 0x10000 else 0]
        out.append('group %d %s' % (g, ' '.join(map(str, v))))
    for rl, gl in zip(raw['lines'], geo['lines']):
        b = bytes.fromhex(rl['raw_hex'])
        out.append('line %d %d %d %d %d %d %d %02x %02x %02x %08x' % (
            gl['index'], *gl['vertices'], *gl['neighbors'], b[0xC], b[0xD], b[0xE], gl['runtime_flags']))
    for v in geo['vertices']:
        out.append('vert %d %s %s' % (v['index'], ' '.join(v['current']['bits_hex']), ' '.join(v['original_bits_hex'])))
    return out, len(geo['lines']), len(geo['vertices'])


def word_lines(what, words):
    return ['%s %x %s' % (what, i * 4, ' '.join('%08x' % w for w in words[i:i + 8])) for i in range(0, len(words), 8)]


def build(archive, stage_id, code, raw_dir, common):
    attr, special = fighter_words(raw_dir, code)
    # ftCo_800D0EC8: -common 0x310 scaled by the player's size (1 here).
    soft = struct.unpack('>I', struct.pack('>f', -f32(common[0x310 // 4])))[0]
    geo, line_n, vert_n = stage_lines(archive)
    lines = ['begin stage %d char %d' % (stage_id, CHARS[code][1])]
    lines += word_lines('common', common) + word_lines('attr', attr) + word_lines('special', special)
    lines += ['softland %08x' % soft] + geo + ['end %d lines %d verts' % (line_n, vert_n)]
    return ''.join('LLDUMP %s\n' % l for l in lines)


def parse_dump(text):
    words, rows = {}, []
    for line in text.splitlines():
        t = line.split('LLDUMP ', 1)
        if len(t) < 2:
            continue
        p = t[1].split()
        if p[0] in ('common', 'attr', 'special'):
            for i, w in enumerate(p[2:]):
                words[(p[0], int(p[1], 16) + i * 4)] = w
        else:
            rows.append(' '.join(p))
    return words, rows


def check(stages_json, raw_dir, game_dump):
    text = open(game_dump).read()
    begin = [l for l in text.splitlines() if 'LLDUMP begin' in l][0].split()
    stage_id, kind = int(begin[3]), int(begin[5])
    archive = [a for a in json.load(open(stages_json))['archives']
               if STAGES.get(a['archive'], ('', -1))[1] == stage_id][0]
    code = [c for c, (_, k) in CHARS.items() if k == kind][0]
    gw, grows = parse_dump(text)
    bw, brows = parse_dump(build(archive, stage_id, code, raw_dir, common_words(raw_dir)))
    bad = 0
    for key in sorted(set(gw) | set(bw)):
        if gw.get(key) != bw.get(key):
            bad += 1
            if bad <= 20:
                print('word %s %x: game %s files %s' % (key[0], key[1], gw.get(key), bw.get(key)))
    for g, b in zip(grows, brows):
        if g != b:
            bad += 1
            if bad <= 40:
                print('row: game "%s" files "%s"' % (g, b))
    if len(grows) != len(brows):
        bad += 1
        print('rows: game %d files %d' % (len(grows), len(brows)))
    print('%s %s: %d differences' % (archive['archive'], CHARS[code][0], bad))
    return bad == 0


def main(argv):
    if argv[:1] == ['--check']:
        return 0 if check(*argv[1:4]) else 1
    if len(argv) < 3:
        print(__doc__)
        return 2
    stages_json, raw_dir, out_dir = argv[:3]
    rest = argv[3:]
    codes = list(CHARS)
    if '--chars' in rest:
        i = rest.index('--chars')
        codes = rest[i + 1].split(',')
        rest = rest[:i] + rest[i + 2:]
    want = rest or list(STAGES)
    common = common_words(raw_dir)
    archives = {a['archive']: a for a in json.load(open(stages_json))['archives']}
    os.makedirs(out_dir, exist_ok=True)
    for gr in want:
        name, stage_id = STAGES[gr]
        for code in codes:
            path = os.path.join(out_dir, '%s_%s.dump' % (name, CHARS[code][0]))
            with open(path, 'w') as f:
                f.write(build(archives[gr], stage_id, code, raw_dir, common))
    print('wrote %d dumps to %s' % (len(want) * len(codes), out_dir))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
