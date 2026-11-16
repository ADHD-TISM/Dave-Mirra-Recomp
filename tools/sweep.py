#!/usr/bin/env python3
"""Coverage sweep: drive the game through every mode, level and menu with
scripted input, letting tools/iterate.sh add any function entry that turns out
to be missing. Results go to testrun/sweep.log, screenshots to testrun/shots/.

usage: python tools/sweep.py [options] <absolute path to image.cue> [seconds per run] [name filter] [pass number]

The game is the one gen/ was built for (gen/game.h). Options:
  --card FILE   start every run from a copy of this memory card (a save with levels unlocked).
                The game loads the card first, so the menu opens later (--shift is raised to match).
  --unlocked    with --card: one run per unlocked level, picked with the level icon (UNLOCKED below),
                instead of the base scenarios. Maximum Remix cannot be told which level to play,
                so this is how it reaches levels other than the first.
  --shift N     frames to add to every menu input (default: the game's own, see GAMES)
  --probe MODE [N]   instead of sweeping: stop on the level icon of MODE (proquest, session,
                freeride), press R N times and write testrun/sheet.png, a contact sheet of the level
                list (locked levels show as LOCKED). Needs Pillow.
Menu timing is tuned for a normal start; it moves with the machine's disc and card speed, so look at
testrun/shots/ the first time you use a new card.
"""
import os, random, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN = os.path.join(ROOT, 'testrun')
LEVEL_BYTE = '80089f54'          # Freestyle BMX: selected level, written just after the level is accepted
# per game: frames its main menu takes to become active beyond the first game's; whether the selected level can
# be poked; frames a loaded memory card adds before the menu
GAMES = {'dmbmx': dict(shift=0, poke=True, card=1600), 'maxremix': dict(shift=400, poke=False, card=1600)}
# --unlocked: R presses from the level the card remembers. Read off the test save with --probe; 4 and 7 on are
# locked on it. Every mode starts from the same remembered level.
UNLOCKED = {'maxremix': [0, 1, 2, 3, 5, 6]}
NAV = {'proquest': [], 'session': ['d'], 'freeride': ['d', 'd'], '2player': ['d', 'd', 'd'],
       'loadsave': ['d'] * 4, 'cool': ['d'] * 5, 'options': ['u']}
PLAY = ['x'] * 6 + ['l', 'r', 'o', 's', 't', 'u', 'd', 'r1', 'l1', 'r2', 'l2']
MENU = ['d', 'd', 'u', 'x', 'x', 'x', 'l', 'r', 't', 'o', 'st']


def game_id():
    try:
        for l in open(os.path.join(ROOT, 'gen', 'game.h')):
            if l.startswith('#define GAME_ID'): return l.split('"')[1]
    except OSError: pass
    return 'dmbmx'


def level_run(mode, level, riders, post, seed, poke=True, lsteps=0):
    """Menu -> rider -> level -> a full run with pause-menu visits -> the Run Finished menu.
    level is poked into memory (poke) or reached with lsteps presses of R on the level icon."""
    rnd = random.Random(seed)
    nav = NAV[mode]; b = 60 * len(nav)
    ev = ['760:se'] + ['%d:%s' % (790 + 60 * i, k) for i, k in enumerate(nav)]
    ev += ['%d:x' % (850 + b)] + ['%d:r' % (970 + b + 40 * i) for i in range(riders)]
    # the accept press moves later when R presses go before it
    late = max(2100, 1950 + 70 * lsteps) - 1900 if lsteps else 0   # R presses push the accept and everything after it later
    ev += ['%d:x' % (t + b + (late if t >= 1900 else 0)) for t in (1100, 1350, 1600, 1900, 2300, 2600, 2900)]
    ev += ['%d:r' % (1750 + b + 70 * i) for i in range(lsteps)]   # level icon: R moves through the levels, locked ones included
    f = 3100 + b + late
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
    args = ['--poke', '%d:%s:%x' % (1903 + b, LEVEL_BYTE, level)] if poke and level is not None else []
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


def scenarios(game, p=0):
    """Pass 0 is the base sweep. Higher passes use other input seeds, riders and
    Run Finished choices, and the levels pass 0 left out in Session and Freeride."""
    tag = '_p%d' % p if p else ''
    k = 1000 * p
    poke = GAMES[game]['poke']
    if poke:   # Freestyle BMX: every level can be selected
        pq = range(13)
        ses = (0, 3, 6, 9, 12) if p % 2 == 0 else (1, 2, 4, 5, 7, 8, 10, 11)
        fre = (1, 4, 7, 10) if p % 2 == 0 else (0, 2, 3, 5, 6, 8, 9, 11, 12)
    else:      # the level is not selectable: first level of each mode, with other seeds
        pq = range(4)
        ses = (0, 3) if p % 2 == 0 else (1, 2)
        fre = (1, 4) if p % 2 == 0 else (0, 2)
    for n in pq: yield 'proquest_L%x%s' % (n, tag), level_run('proquest', n, (n + p) % 5, (n + 3 * p) % 6, 100 + n + k, poke)
    for n in ses: yield 'session_L%x%s' % (n, tag), level_run('session', n, (n + 2 + p) % 5, (n + 1 + p) % 6, 200 + n + k, poke)
    for n in fre: yield 'freeride_L%x%s' % (n, tag), level_run('freeride', n, (n + 3 + p) % 5, (n + 4 + p) % 6, 300 + n + k, poke)
    for i in range(3): yield '2player_%d%s' % (i, tag), menu_run('2player', 400 + i + k)
    for i in range(2): yield 'loadsave_%d%s' % (i, tag), menu_run('loadsave', 500 + i + k)
    for i in range(2): yield 'cool_%d%s' % (i, tag), menu_run('cool', 600 + i + k)
    for i in range(2): yield 'options_%d%s' % (i, tag), menu_run('options', 700 + i + k)
    if p == 0: yield 'attract', (['999999:x'], ['--auto-start', '0'])


def unlocked(game, p=0):
    """One run per unlocked level of the card, in every mode that has a level icon."""
    for mode in ('proquest', 'session', 'freeride'):
        for st in UNLOCKED.get(game, [0]):
            yield '%s_U%d_p%d' % (mode, st, p), level_run(mode, None, (st + p) % 5, (st + 2 * p) % 6, 800 + st + 17 * p + 1000 * p, False, st)


def prepare(ev, shift, card):
    """Menu inputs wait for the menu: shift them, and start from a fresh copy of the card."""
    ev = [e if int(e.split(':')[0]) < 790 else '%d:%s' % (int(e.split(':')[0]) + shift, e.split(':')[1]) for e in ev]
    data = open(card, 'rb').read() if card else None   # read first: the card may be one of the files cleared below
    for f in os.listdir(RUN):
        if f.startswith('shot_') or f.endswith('.mcd'): os.remove(os.path.join(RUN, f))
    if data is not None: open(os.path.join(RUN, 'memcard1.mcd'), 'wb').write(data)
    open(os.path.join(RUN, 's.txt'), 'w').write(','.join(ev))


def probe(cue, mode, n, shift, card):
    """Stop on the level icon, press R n times and write a contact sheet of what the picker shows."""
    try: from PIL import Image
    except ImportError: sys.exit('--probe needs Pillow (pip install pillow)')
    ev, _ = level_run(mode, None, 0, 0, 1, False, n)
    t = lambda e: int(e.split(':')[0])
    b = 60 * len(NAV[mode])
    ev = [e for e in ev if t(e) < 1650 + b + 60 or (e.endswith(':r') and t(e) < 1750 + b + 70 * n + 60)]   # no accept press
    os.makedirs(RUN, exist_ok=True)
    prepare(ev, shift, card)
    t0 = 1650 + b + shift - 100
    try:
        subprocess.run([os.path.join(ROOT, 'build', 'dmrecomp.exe'), '--turbo', '--auto-start', '90', '--shot-every', '40',
                        '--script-file', 's.txt', cue], cwd=RUN, stdout=open(os.path.join(RUN, 'run.log'), 'w'),
                       stderr=subprocess.STDOUT, timeout=16)
    except subprocess.TimeoutExpired: pass
    fr = [f for f in sorted(os.listdir(RUN)) if f.startswith('shot_') and t0 <= int(f[5:10]) < t0 + 70 * n + 500]
    if not fr: sys.exit('no screenshots were taken')
    W, H = 256, 120
    im = Image.new('RGB', (W * 4, H * ((len(fr) + 3) // 4)))
    for i, f in enumerate(fr):
        im.paste(Image.open(os.path.join(RUN, f)).convert('RGB').resize((W, H)), ((i % 4) * W, (i // 4) * H))
    im.save(os.path.join(RUN, 'sheet.png')); print('testrun/sheet.png:', len(fr), 'frames')


def main(argv):
    opts = {'card': None, 'unlocked': False, 'shift': None, 'probe': None}
    pos = []
    it = iter(argv)
    for a in it:
        if a == '--card': opts['card'] = os.path.abspath(next(it))
        elif a == '--unlocked': opts['unlocked'] = True
        elif a == '--shift': opts['shift'] = int(next(it))
        elif a == '--probe': opts['probe'] = next(it)
        elif a in ('-h', '--help'): print(__doc__); return 0
        else: pos.append(a)
    if not pos: print(__doc__); return 2
    game = game_id()
    if game not in GAMES: sys.exit('no sweep settings for game %s' % game)
    if opts['card'] and not os.path.isfile(opts['card']): sys.exit('no such card: %s' % opts['card'])
    if opts['unlocked'] and not opts['card']: sys.exit('--unlocked needs --card')
    cue = pos[0]
    shift = opts['shift'] if opts['shift'] is not None else GAMES[game]['shift'] + (GAMES[game]['card'] if opts['card'] else 0)
    os.makedirs(RUN, exist_ok=True)
    if opts['probe']:
        probe(cue, opts['probe'], int(pos[1]) if len(pos) > 1 else 10, shift, opts['card'])
        return 0
    secs = pos[1] if len(pos) > 1 else '60'; only = pos[2] if len(pos) > 2 else ''; npass = int(pos[3]) if len(pos) > 3 else 0
    log = open(os.path.join(RUN, 'sweep.log'), 'a')
    for name, (ev, extra) in (unlocked(game, npass) if opts['unlocked'] else scenarios(game, npass)):
        if only and only not in name: continue
        prepare(ev, shift, opts['card'])
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
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
