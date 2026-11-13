#!/usr/bin/env python3
"""Coverage sweep: drive the game through every mode, level and menu with
scripted input, letting tools/iterate.sh add any function entry that turns out
to be missing. Results go to testrun/sweep.log, screenshots to testrun/shots/.

usage: python tools/sweep.py <absolute path to image.cue> [seconds per run] [name filter] [pass number]
"""
import os, random, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN = os.path.join(ROOT, 'testrun')
LEVEL_BYTE = '80089f54'          # selected level, written just after the level is accepted
NAV = {'proquest': [], 'session': ['d'], 'freeride': ['d', 'd'], '2player': ['d', 'd', 'd'],
       'loadsave': ['d'] * 4, 'cool': ['d'] * 5, 'options': ['u']}
PLAY = ['x'] * 6 + ['l', 'r', 'o', 's', 't', 'u', 'd', 'r1', 'l1', 'r2', 'l2']
MENU = ['d', 'd', 'u', 'x', 'x', 'x', 'l', 'r', 't', 'o', 'st']


def level_run(mode, level, riders, post, seed):
    """Menu -> rider -> level -> a full run with pause-menu visits -> the Run Finished menu."""
    rnd = random.Random(seed)
    nav = NAV[mode]; b = 60 * len(nav)
    ev = ['760:se'] + ['%d:%s' % (790 + 60 * i, k) for i, k in enumerate(nav)]
    ev += ['%d:x' % (850 + b)] + ['%d:r' % (970 + b + 40 * i) for i in range(riders)]
    ev += ['%d:x' % (t + b) for t in (1100, 1350, 1600, 1900, 2300, 2600, 2900)]
    f = 3100 + b
    next_pause = f + rnd.choice([2500, 4000, 6000])
    while f < 17000 + b:
        if f >= next_pause:
            ev.append('%d:st' % f); f += 90
            for _ in range(rnd.randint(2, 6)):
                ev.append('%d:%s' % (f, rnd.choice(['d', 'u', 'x', 't', 'd']))); f += 80
            ev.append('%d:st' % f); f += 90
            next_pause = f + rnd.choice([3000, 5000, 9000])
        ev.append('%d:%s' % (f, rnd.choice(PLAY))); f += rnd.choice([8, 15, 30])
    f = 17700 + b
    for _ in range(post):
        ev.append('%d:d' % f); f += 60
    ev.append('%d:x' % f); f += 250
    while f < 30000:
        ev.append('%d:%s' % (f, rnd.choice(MENU))); f += rnd.choice([60, 120, 200])
    args = ['--poke', '%d:%s:%x' % (1903 + b, LEVEL_BYTE, level)] if level is not None else []
    return ev, args


def menu_run(mode, seed):
    """Open a main-menu entry and wander around it."""
    rnd = random.Random(seed)
    nav = NAV[mode]; b = 60 * len(nav)
    ev = ['760:se'] + ['%d:%s' % (790 + 60 * i, k) for i, k in enumerate(nav)] + ['%d:x' % (850 + b)]
    f = 1100 + b
    pool = (['x'] * 8 + PLAY + ['d', 'l', 'r', 'st']) if mode == '2player' else MENU
    while f < 30000:
        ev.append('%d:%s' % (f, rnd.choice(pool))); f += rnd.choice([40, 80, 150])
    return ev, (['--set', 'player2=2'] if mode == '2player' else [])


def scenarios(p=0):
    """Pass 0 is the base sweep. Higher passes use other input seeds, riders and
    Run Finished choices, and the levels pass 0 left out in Session and Freeride."""
    tag = '_p%d' % p if p else ''
    k = 1000 * p
    for n in range(13): yield 'proquest_L%x%s' % (n, tag), level_run('proquest', n, (n + p) % 5, (n + 3 * p) % 6, 100 + n + k)
    ses = (0, 3, 6, 9, 12) if p % 2 == 0 else (1, 2, 4, 5, 7, 8, 10, 11)
    fre = (1, 4, 7, 10) if p % 2 == 0 else (0, 2, 3, 5, 6, 8, 9, 11, 12)
    for n in ses: yield 'session_L%x%s' % (n, tag), level_run('session', n, (n + 2 + p) % 5, (n + 1 + p) % 6, 200 + n + k)
    for n in fre: yield 'freeride_L%x%s' % (n, tag), level_run('freeride', n, (n + 3 + p) % 5, (n + 4 + p) % 6, 300 + n + k)
    for i in range(3): yield '2player_%d%s' % (i, tag), menu_run('2player', 400 + i + k)
    for i in range(2): yield 'loadsave_%d%s' % (i, tag), menu_run('loadsave', 500 + i + k)
    for i in range(2): yield 'cool_%d%s' % (i, tag), menu_run('cool', 600 + i + k)
    for i in range(2): yield 'options_%d%s' % (i, tag), menu_run('options', 700 + i + k)
    if p == 0: yield 'attract', (['999999:x'], ['--auto-start', '0'])


def main():
    cue = sys.argv[1]; secs = sys.argv[2] if len(sys.argv) > 2 else '60'; only = sys.argv[3] if len(sys.argv) > 3 else ''; npass = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    os.makedirs(RUN, exist_ok=True)
    log = open(os.path.join(RUN, 'sweep.log'), 'a')
    for name, (ev, extra) in scenarios(npass):
        if only and only not in name: continue
        for f in os.listdir(RUN):
            if f.startswith('shot_') or f.endswith('.mcd'): os.remove(os.path.join(RUN, f))
        open(os.path.join(RUN, 's.txt'), 'w').write(','.join(ev))
        env = dict(os.environ, DM_ARGS=' '.join(['--turbo', '--auto-start', '90', '--stats', '--shot-every', '3000',
                                                 '--script-file', 's.txt'] + extra))
        r = subprocess.run(['sh', 'tools/iterate.sh', cue, secs, '12'], cwd=ROOT, env=env, capture_output=True, text=True)
        out = ' | '.join(l for l in r.stdout.strip().splitlines())
        secs_run = sum(1 for l in open(os.path.join(RUN, 'run.log'), errors='replace') if l.startswith('[time]'))
        line = '%-14s emulated=%ds  %s' % (name, secs_run, out)
        print(line, flush=True); log.write(line + '\n'); log.flush()
        dst = os.path.join(RUN, 'shots', name); shutil.rmtree(dst, ignore_errors=True); os.makedirs(dst)
        for f in os.listdir(RUN):
            if f.startswith('shot_'): shutil.move(os.path.join(RUN, f), os.path.join(dst, f))


if __name__ == '__main__':
    main()
