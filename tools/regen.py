#!/usr/bin/env python3
"""Translate a game's code to C from its extracted disc, using a game profile.

usage: python tools/regen.py [--game ID] <extracted disc dir> [out dir]

The profile (games/<id>.json) names the executable, the overlays and data files
that go with it, the overlay load address and the entry-point lists. Without
--game, the game is the one whose executable is on the disc. Iterates so that
main-EXE functions only reachable from overlays become entries too. Also writes
<out>/game.h, which the runtime includes to learn which game it was built for.
"""
import glob, json, os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_profiles():
    out = {}
    for p in sorted(glob.glob(os.path.join(ROOT, 'games', '*.json'))):
        with open(p, encoding='utf-8') as f:
            g = json.load(f)
        out[g['id']] = g
    return out


def identify(disc_dir, profiles):
    for g in profiles.values():
        if os.path.isfile(os.path.join(disc_dir, g['exe'])):
            return g
    return None


def lines(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            return [l.rstrip('\r\n') for l in f]
    except OSError:
        return []


def main(argv):
    game_id = None
    args = []
    it = iter(argv)
    for a in it:
        if a == '--game':
            game_id = next(it, None)
        else:
            args.append(a)
    if not args:
        print(__doc__)
        return 2
    disc = os.path.abspath(args[0])
    gen = os.path.abspath(args[1]) if len(args) > 1 else os.path.join(ROOT, 'gen')
    profiles = load_profiles()
    g = profiles.get(game_id) if game_id else identify(disc, profiles)
    if not g:
        print('no game profile matches %s (games: %s)' % (game_id or disc, ', '.join(profiles) or 'none'))
        return 1
    d = lambda n: os.path.join(disc, n)
    exe = d(g['exe'])
    if not os.path.isfile(exe):
        print('not found: %s' % exe)
        return 1
    ex = lambda m: os.path.join(ROOT, g['extras'], 'extra_%s.txt' % m)
    py = [sys.executable, '-I', os.path.join(ROOT, 'tools', 'recomp.py')]
    base = g['overlay_base']
    ovl = g['overlays']
    shutil.rmtree(gen, ignore_errors=True)
    os.makedirs(gen)
    xref = os.path.join(gen, 'xref_main.txt')
    open(xref, 'w').close()
    scan = []
    for o in ovl.values():
        scan += ['--scan', d(o['file']), '--scan', d(o['data'])]
    cost = []
    for a in g.get('fullcost', []):
        cost += ['--fullcost', a]

    def run(cmd):
        subprocess.run(cmd, check=True, cwd=ROOT)

    prev = -1
    for _ in range(8):
        run(py + [exe, 'main', gen] + scan + ['--extra', ex('main'), '--extra', xref] + cost)
        for name, o in ovl.items():
            run(py + [d(o['file']), name, gen, '--raw', base, '--scan', exe, '--scan', d(o['data']),
                      '--roots', os.path.join(gen, 'main_unresolved.txt'), '--extra', ex(name),
                      '--extern', os.path.join(gen, 'main_entries.txt')])
        keep = set()
        for src in [os.path.join(gen, '%s_unresolved.txt' % n) for n in ovl] + [xref]:
            for l in lines(src):
                f = l.split()
                if f and f[0] < base:   # entries below the overlay area, as a string compare like the old awk
                    keep.add(l)
        with open(xref, 'w') as f:
            f.write(''.join(l + '\n' for l in sorted(keep)))
        n = len(lines(os.path.join(gen, 'main_entries.txt')))
        if n == prev:
            break
        prev = n
    else:
        print('code translation did not settle after 8 passes')
        return 1
    run(py + [d(g['movie']), 'movie', gen, '--extra', ex('movie')])
    with open(os.path.join(gen, 'game.h'), 'w') as f:
        f.write('/* written by tools/regen.py */\n#define GAME_ID "%s"\n#define GAME_NAME "%s"\n'
                '#define GAME_SERIAL "%s"\n#define GAME_EXE "%s"\n'
                '#define GAME_FILE "%s"   /* the name as it can appear in a Windows file name */\n'
                % (g['id'], g['name'], g['serial'], g['exe'], g['name'].replace(':', ' -')))
    print('translated %s (%s)' % (g['name'], g['serial']))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
