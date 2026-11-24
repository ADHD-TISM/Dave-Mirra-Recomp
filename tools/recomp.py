"""Static recompiler: PS-X EXE (MIPS R3000A) -> C.

usage: python recomp.py <input> <prefix> <out_dir> [--raw HEXBASE] [--extra FILE] [--roots FILE]
       [--scan FILE] [--extern FILE] [--fullcost HEXENTRY]    (options repeat; see tools/regen.sh)
       [--patches games/<id>.json]

Each discovered entry point becomes one C function `f_XXXXXXXX(void)` that
contains every instruction reachable from that entry by local control flow
(code shared between functions is simply duplicated). Branches become goto,
`jal` becomes a native call, `jr ra` becomes return, `j` to another entry is
a tail call, `jr reg` resolves recovered jump tables and otherwise goes
through the runtime dispatcher (`psx_jump`). Guest registers live in the
global `cpu` struct; memory goes through the inline accessors in psx.h.
"""
import os, struct, sys

REG = ['zero','at','v0','v1','a0','a1','a2','a3','t0','t1','t2','t3','t4','t5','t6','t7',
       's0','s1','s2','s3','s4','s5','s6','s7','t8','t9','k0','k1','gp','sp','fp','ra']

def s16(x): return x - 0x10000 if x & 0x8000 else x

class Exe:
    def __init__(self, path, raw_base=None):
        d = open(path, 'rb').read()
        if raw_base is None:
            assert d[:8] == b'PS-X EXE'
            self.pc0, = struct.unpack_from('<I', d, 0x10)
            self.base, self.size = struct.unpack_from('<II', d, 0x18)
            self.data = d[0x800:0x800 + self.size]
        else:
            self.pc0 = None
            self.base, self.size = raw_base, len(d) & ~3
            self.data = d[:self.size]
        self.end = self.base + self.size
    def word(self, a):
        o = a - self.base
        if o < 0 or o + 4 > len(self.data): return None
        return struct.unpack_from('<I', self.data, o)[0]
    def inside(self, a): return self.base <= a < self.end and (a & 3) == 0

class Insn:
    __slots__ = ('a','w','op','rs','rt','rd','sa','fn','imm','simm','tgt')
    def __init__(self, a, w):
        self.a, self.w = a, w
        self.op = w >> 26; self.rs = (w >> 21) & 31; self.rt = (w >> 16) & 31
        self.rd = (w >> 11) & 31; self.sa = (w >> 6) & 31; self.fn = w & 63
        self.imm = w & 0xffff; self.simm = s16(self.imm)
        self.tgt = ((a + 4) & 0xf0000000) | ((w & 0x3ffffff) << 2)

# --- classification ---------------------------------------------------------
def kind(i):
    """returns one of: None, 'b' (cond branch), 'bal' (cond call), 'j', 'jal',
    'jr', 'jalr'"""
    if i.op == 0:
        if i.fn == 8: return 'jr'
        if i.fn == 9: return 'jalr'
        return None
    if i.op == 1:
        if i.rt & 0x1e == 0x10: return 'bal'   # bltzal/bgezal
        return 'b'
    if i.op == 2: return 'j'
    if i.op == 3: return 'jal'
    if 4 <= i.op <= 7: return 'b'
    return None

def btarget(i): return (i.a + 4 + (i.simm << 2)) & 0xffffffff

VALID_OPS = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,18,32,33,34,35,36,37,38,40,41,42,43,46,50,58}
VALID_SPECIAL = {0,2,3,4,6,7,8,9,12,13,16,17,18,19,24,25,26,27,32,33,34,35,36,37,38,39,42,43}

def valid(i):
    if i.op not in VALID_OPS: return False
    if i.op == 0 and i.fn not in VALID_SPECIAL: return False
    return True

# --- analysis ---------------------------------------------------------------
class Func:
    def __init__(self, entry):
        self.entry = entry
        self.addrs = set()
        self.labels = set()
        self.tables = {}   # jr address -> list of targets
        self.calls = set()
        self.ptrs = set()

def find_jump_table(exe, i):
    """Recover a GCC switch table feeding `jr rX` at i.a. Looks back for
    `lw rX, lo(rB)`, a `sltiu` bound, and the `lui` (+ optional `addiu` on the
    same register) that forms the table address; the lui may sit in the delay
    slot of the bounds branch and reach rB through an addu."""
    reg = i.rs
    lw = None; n = None; luis = []; addius = {}
    a = i.a - 4
    for _ in range(20):
        w = exe.word(a)
        if w is None: break
        j = Insn(a, w)
        if lw is None:
            if j.op == 35 and j.rt == reg: lw = j
        else:
            if j.op == 15: luis.append(j)
            elif j.op == 9 and j.rs == j.rt: addius.setdefault(j.rt, j)
            elif j.op == 11 and n is None: n = j.imm
        a -= 4
    if lw is None or not n: return None
    best = None
    for l in luis:
        lo = addius[l.rt].simm if l.rt in addius and addius[l.rt].a > l.a else 0
        base = ((l.imm << 16) + lo + lw.simm) & 0xffffffff
        out = []
        for idx in range(n):
            t = exe.word(base + 4 * idx)
            if t is None or not exe.inside(t) or abs(t - i.a) > 0x20000: break
            out.append(t)
        if len(out) == n and (best is None or len(out) > len(best)): best = out
    if best: return best
    # table base kept in a register set up much earlier: lw rX,lo(rB) fed by
    # addu rB, rIdx, rBase with rBase = lui/addiu far above.
    cands = {lw.rs}
    a = lw.a - 4
    for _ in range(6):
        w = exe.word(a)
        if w is None: break
        j = Insn(a, w)
        if j.op == 0 and (w & 0x7ff) == 0x21 and ((w >> 11) & 31) == lw.rs:
            cands |= {j.rs, j.rt}; break
        a -= 4
    a = lw.a - 4
    for _ in range(600):
        w = exe.word(a)
        if w is None: break
        j = Insn(a, w)
        if j.op == 9 and j.rt in cands and j.rs != 0 and j.rs != 29:
            for b in range(a - 4, a - 24, -4):
                w2 = exe.word(b)
                if w2 is None: break
                l = Insn(b, w2)
                if l.op == 15 and l.rt == j.rs:
                    base = ((l.imm << 16) + j.simm + lw.simm) & 0xffffffff
                    out = []
                    for idx in range(n):
                        t = exe.word(base + 4 * idx)
                        if t is None or not exe.inside(t) or abs(t - i.a) > 0x20000: break
                        out.append(t)
                    if len(out) == n: return out
                    break
        a -= 4
    return None

def analyse(exe, entry, entries):
    f = Func(entry)
    work = [entry]
    while work:
        a = work.pop()
        while True:
            if a in f.addrs or not exe.inside(a): break
            w = exe.word(a); i = Insn(a, w)
            f.addrs.add(a)
            if not valid(i): break
            # remember lui/addiu-style code pointers
            if i.op == 15:
                nw = exe.word(a + 4)
                if nw is not None:
                    n = Insn(a + 4, nw)
                    if n.op in (9, 13) and n.rs == i.rt:
                        p = (i.imm << 16) + (n.simm if n.op == 9 else n.imm)
                        f.ptrs.add(p & 0xffffffff)
            k = kind(i)
            if k is None:
                a += 4; continue
            f.addrs.add(a + 4)  # delay slot
            if k == 'b':
                t = btarget(i); f.labels.add(t); work.append(t)
                # beq zero,zero is unconditional
                if i.op == 4 and i.rs == 0 and i.rt == 0: break
                a += 8; continue
            if k == 'bal':
                f.calls.add(btarget(i))
                a += 8; continue
            if k == 'jal':
                f.calls.add(i.tgt); a += 8; continue
            if k == 'jalr':
                a += 8; continue
            if k == 'j':
                t = i.tgt
                if t != entry and t in entries and t not in WEAK:
                    break  # tail call
                f.labels.add(t); work.append(t); break
            if k == 'jr':
                if i.rs != 31:
                    tab = find_jump_table(exe, i)
                    if tab:
                        f.tables[a] = tab
                        for t in tab:
                            f.labels.add(t); work.append(t)
                break
    # a label on a delay slot needs the following instruction too
    return f

WEAK = set()

def plausible_start(exe, t):
    if not exe.inside(t): return False
    w = exe.word(t)
    if w is None or not valid(Insn(t, w)): return False
    if t == exe.base: return True
    if exe.word(t - 8) == 0x03e00008: return True          # after jr ra; delay
    if (w >> 16) == 0x27bd and (w & 0x8000): return True   # addiu sp,sp,-N
    return False

def discover(exe, extra, roots=()):
    entries = set(e for e in extra if exe.inside(e))
    entries |= set(e for e in roots if plausible_start(exe, e))
    if exe.pc0 is not None: entries.add(exe.pc0)
    funcs = {}
    # data words that look like code pointers
    data_ptrs = set()
    for o in range(0, len(exe.data) - 3, 4):
        v = struct.unpack_from('<I', exe.data, o)[0]
        if exe.inside(v) and plausible_start(exe, v):
            data_ptrs.add(v)
    entries |= data_ptrs
    # linear sweep: whatever follows `jr ra; <delay>` (after nop padding) is
    # taken as a function start. Swept entries that no `jal` targets are
    # "weak": a `j` to one is still inlined, so a loop head that happens to
    # follow an early return does not turn into host recursion.
    swept = set()
    a = exe.base + 8
    while a < exe.end:
        if exe.word(a - 8) == 0x03e00008:
            t = a
            while exe.word(t) == 0 and t < exe.end - 4: t += 4
            w = exe.word(t)
            if w is not None and valid(Insn(t, w)) and t not in entries:
                entries.add(t); swept.add(t)
        a += 4
    changed = True
    while changed:
        changed = False
        for e in sorted(entries):
            if e in funcs: continue
            f = analyse(exe, e, entries)
            funcs[e] = f
            new = set(c for c in f.calls if exe.inside(c))
            new |= set(p for p in f.ptrs if plausible_start(exe, p))
            if new - entries:
                entries |= new; changed = True
    # entries added after a function was analysed can turn `j` targets into
    # tail calls; re-analyse everything once against the final entry set.
    called = set()
    for f in funcs.values(): called |= f.calls
    WEAK.update(swept - called)
    for e in sorted(entries):
        funcs[e] = analyse(exe, e, entries)
    return funcs

# --- code generation --------------------------------------------------------
def R(n): return '0' if n == 0 else f'R[{n}]'

# Instruction patches, from the "patches" of games/<id>.json (--patches): address -> (the C the instruction gives,
# the C to emit instead). The first part is checked, so a patch never lands on a different instruction.
PATCHES = {}
PATCHED = set()

def emit_simple(i):
    """C for a non-branch instruction (returns string, may be empty), with the patches applied."""
    s = emit_plain(i)
    p = PATCHES.get(i.a)
    if p is None: return s
    if s != p[0]:
        raise SystemExit(f'patch at {i.a:08x}: the instruction gives {s!r}, not {p[0]!r}')
    PATCHED.add(i.a)
    return p[1]

def emit_plain(i):
    """C for a non-branch instruction (returns string, may be empty)."""
    op, rs, rt, rd, sa, fn = i.op, i.rs, i.rt, i.rd, i.sa, i.fn
    imm, simm = i.imm, i.simm
    def setd(r, expr):
        return f'{R(r)} = {expr};' if r else ''
    if op == 0:
        if i.w == 0: return ''
        if fn == 0:  return setd(rd, f'{R(rt)} << {sa}')
        if fn == 2:  return setd(rd, f'{R(rt)} >> {sa}')
        if fn == 3:  return setd(rd, f'(u32)((s32){R(rt)} >> {sa})')
        if fn == 4:  return setd(rd, f'{R(rt)} << ({R(rs)} & 31)')
        if fn == 6:  return setd(rd, f'{R(rt)} >> ({R(rs)} & 31)')
        if fn == 7:  return setd(rd, f'(u32)((s32){R(rt)} >> ({R(rs)} & 31))')
        if fn == 12: return f'psx_syscall(0x{i.a:08x});'
        if fn == 13: return f'psx_break(0x{i.a:08x}, {(i.w >> 6) & 0xfffff});'
        if fn == 16: return setd(rd, 'cpu.hi')
        if fn == 17: return f'cpu.hi = {R(rs)};'
        if fn == 18: return setd(rd, 'cpu.lo')
        if fn == 19: return f'cpu.lo = {R(rs)};'
        if fn == 24: return f'{{ s64 m = (s64)(s32){R(rs)} * (s32){R(rt)}; cpu.lo = (u32)m; cpu.hi = (u32)(m >> 32); }}'
        if fn == 25: return f'{{ u64 m = (u64){R(rs)} * {R(rt)}; cpu.lo = (u32)m; cpu.hi = (u32)(m >> 32); }}'
        if fn == 26: return f'psx_div({R(rs)}, {R(rt)});'
        if fn == 27: return f'psx_divu({R(rs)}, {R(rt)});'
        if fn in (32, 33): return setd(rd, f'{R(rs)} + {R(rt)}')
        if fn in (34, 35): return setd(rd, f'{R(rs)} - {R(rt)}')
        if fn == 36: return setd(rd, f'{R(rs)} & {R(rt)}')
        if fn == 37: return setd(rd, f'{R(rs)} | {R(rt)}')
        if fn == 38: return setd(rd, f'{R(rs)} ^ {R(rt)}')
        if fn == 39: return setd(rd, f'~({R(rs)} | {R(rt)})')
        if fn == 42: return setd(rd, f'(s32){R(rs)} < (s32){R(rt)}')
        if fn == 43: return setd(rd, f'{R(rs)} < {R(rt)}')
    if op in (8, 9):  return setd(rt, f'{R(rs)} + {simm & 0xffffffff:#x}u')
    if op == 10: return setd(rt, f'(s32){R(rs)} < {simm}')
    if op == 11: return setd(rt, f'{R(rs)} < {simm & 0xffffffff:#x}u')
    if op == 12: return setd(rt, f'{R(rs)} & {imm:#x}')
    if op == 13: return setd(rt, f'{R(rs)} | {imm:#x}')
    if op == 14: return setd(rt, f'{R(rs)} ^ {imm:#x}')
    if op == 15: return setd(rt, f'{(imm << 16):#x}u')
    if op == 16:
        if rs == 0: return setd(rt, f'cop0_read({rd})')
        if rs == 4: return f'cop0_write({rd}, {R(rt)});'
        if rs == 16 and fn == 16: return 'cop0_rfe();'
        return f'/* cop0 {i.w:08x} */'
    if op == 18:
        if rs == 0: return setd(rt, f'gte_read({rd})')
        if rs == 2: return setd(rt, f'gte_readc({rd})')
        if rs == 4: return f'gte_write({rd}, {R(rt)});'
        if rs == 6: return f'gte_writec({rd}, {R(rt)});'
        if rs >= 16: return f'gte_cmd(0x{i.w & 0x1ffffff:07x});'
        return f'/* cop2 {i.w:08x} */'
    ea = f'{R(rs)} + {simm & 0xffffffff:#x}u'
    if op == 32: return (setd(rt, f'(u32)(s32)(s8)mr8({ea})') or f'mr8({ea});')
    if op == 33: return (setd(rt, f'(u32)(s32)(s16)mr16({ea})') or f'mr16({ea});')
    if op == 34: return setd(rt, f'mlwl({ea}, {R(rt)})')
    if op == 35: return (setd(rt, f'mr32({ea})') or f'mr32({ea});')
    if op == 36: return (setd(rt, f'mr8({ea})') or f'mr8({ea});')
    if op == 37: return (setd(rt, f'mr16({ea})') or f'mr16({ea});')
    if op == 38: return setd(rt, f'mlwr({ea}, {R(rt)})')
    if op == 40: return f'mw8({ea}, (u8){R(rt)});'
    if op == 41: return f'mw16({ea}, (u16){R(rt)});'
    if op == 42: return f'mswl({ea}, {R(rt)});'
    if op == 43: return f'mw32({ea}, {R(rt)});'
    if op == 46: return f'mswr({ea}, {R(rt)});'
    if op == 50: return f'gte_write({rt}, mr32({ea}));'
    if op == 58: return f'mw32({ea}, gte_read({rt}));'
    return f'psx_bad(0x{i.a:08x}, 0x{i.w:08x}); return;'

def cond(i):
    rs, rt = R(i.rs), R(i.rt)
    if i.op == 4: return f'{rs} == {rt}'
    if i.op == 5: return f'{rs} != {rt}'
    if i.op == 6: return f'(s32){rs} <= 0'
    if i.op == 7: return f'(s32){rs} > 0'
    if i.op == 1:
        return f'(s32){rs} >= 0' if i.rt & 1 else f'(s32){rs} < 0'
    raise ValueError

def callexpr(t, entries, P, ext):
    if t in entries: return f'{P}_{t:08x}();'
    if t in ext: return f'{ext[t]}_{t:08x}();'
    return f'psx_call(0x{t:08x}u);'

def gen_func(exe, f, entries, out, P, ext):
    a0 = f.entry
    out.append(f'void {P}_{a0:08x}(void) {{')
    addrs = sorted(f.addrs)
    aset = f.addrs
    needed = set(t for t in f.labels if t in aset)
    if addrs and addrs[0] != a0:
        needed.add(a0)
        out.append(f'  goto L_{a0:08x};')
    # count insns since last flush for cycle accounting
    pend = 0
    consumed = set()
    prev = None
    for a in addrs:
        if prev is not None and a != prev + 4:
            # discontinuity: previous chunk must have ended in a terminal;
            # guard anyway.
            out.append(f'  psx_fallthrough(0x{prev + 4:08x}); return;')
        prev = a
        if a in needed:
            if pend: out.append(f'  CYC({pend});'); pend = 0
            out.append(f'L_{a:08x}:;')
        if a in consumed and a not in needed:
            continue
        w = exe.word(a); i = Insn(a, w)
        if a in consumed:
            # reached a delay slot via label: run it standalone and continue
            s = emit_simple(i)
            if s: out.append('  ' + s)
            pend += 1
            continue
        k = kind(i) if valid(i) else None
        if not valid(i):
            out.append(f'  psx_bad(0x{a:08x}, 0x{w:08x}); return;')
            continue
        pend += 1
        if k is None:
            s = emit_simple(i)
            if s: out.append('  ' + s)
            continue
        d = Insn(a + 4, exe.word(a + 4) or 0)
        ds = emit_simple(d) if kind(d) is None else f'/* branch in delay slot {d.w:08x} */'
        consumed.add(a + 4)
        pend += 1
        out.append(f'  CYC({pend});'); pend = 0
        if k == 'b':
            t = btarget(i)
            if i.op == 4 and i.rs == 0 and i.rt == 0:
                out.append(f'  {{ {ds} goto L_{t:08x}; }}')
            else:
                out.append(f'  {{ int c = {cond(i)}; {ds} if (c) goto L_{t:08x}; }}')
        elif k == 'bal':
            t = btarget(i)
            call = callexpr(t, entries, P, ext)
            if i.rs == 0 and i.rt & 1:
                out.append(f'  {{ {ds} R[31] = 0x{a + 8:08x}u; {call} }}')
            else:
                out.append(f'  {{ int c = {cond(i)}; R[31] = 0x{a + 8:08x}u; {ds} if (c) {{ {call} }} }}')
        elif k == 'jal':
            t = i.tgt
            call = callexpr(t, entries, P, ext)
            out.append(f'  {{ {ds} R[31] = 0x{a + 8:08x}u; {call} }}')
        elif k == 'jalr':
            link = f'R[{i.rd}] = 0x{a + 8:08x}u;' if i.rd else ''
            out.append(f'  {{ u32 t = {R(i.rs)}; {ds} {link} psx_call(t); }}')
        elif k == 'j':
            t = i.tgt
            if t in aset and (t == a0 or t not in entries or t in WEAK):
                out.append(f'  {{ {ds} goto L_{t:08x}; }}')
            elif t in entries or t in ext:
                out.append(f'  {{ {ds} {callexpr(t, entries, P, ext)} return; }}')
            else:
                out.append(f'  {{ {ds} psx_call(0x{t:08x}u); return; }}')
        elif k == 'jr':
            if i.rs == 31:
                out.append(f'  {{ {ds} return; }}')
            else:
                tab = f.tables.get(a)
                out.append(f'  {{ u32 t = {R(i.rs)}; {ds}')
                if tab:
                    out.append('    switch (t) {')
                    for t in sorted(set(tab)):
                        out.append(f'    case 0x{t:08x}u: goto L_{t:08x};')
                    out.append('    }')
                out.append('    psx_call(t); return; }')
        if a + 4 in needed and k in ('b', 'bal', 'jal', 'jalr') and a + 8 in aset:
            # the slot at a+4 is a branch target too, so it has its own label and standalone copy;
            # fall-through and call-return paths already ran it above and must skip that copy
            needed.add(a + 8)
            out.append(f'  goto L_{a + 8:08x};')
    if prev is not None:
        out.append(f'  psx_fallthrough(0x{prev + 4:08x});')
    out.append('}')
    out.append('')

NL = chr(10)

def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('input'); ap.add_argument('prefix'); ap.add_argument('out_dir')
    ap.add_argument('--raw', type=lambda x: int(x, 16), help='raw image load base')
    ap.add_argument('--extra', action='append', default=[], help='file of extra entry addresses')
    ap.add_argument('--roots', action='append', default=[], help='candidate entries, kept only if they look like function starts')
    ap.add_argument('--scan', action='append', default=[], help='binary file whose aligned words are candidate entries')
    ap.add_argument('--fullcost', action='append', default=[], type=lambda x: int(x, 16), help='entry whose instructions always cost console time')
    ap.add_argument('--extern', action='append', default=[], help='entries file of an always-resident module')
    ap.add_argument('--patches', help='game profile (games/<id>.json) whose "patches" replace single instructions')
    a = ap.parse_args()
    P = a.prefix
    if a.patches:
        import json, re
        with open(a.patches, encoding='utf-8') as f:
            pt = json.load(f).get('patches', {})
        if not isinstance(pt, dict):
            raise SystemExit(f'{a.patches}: "patches" must be an object of "address": ["the C the instruction gives", "the C to emit instead"]')
        for k, v in pt.items():
            if not re.fullmatch(r'[0-9a-fA-F]{8}', k) or int(k, 16) & 3:
                raise SystemExit(f'patch {k!r}: the address must be 8 hex digits of an instruction (a multiple of 4)')
            if not (isinstance(v, list) and len(v) == 2 and all(isinstance(s, str) for s in v)):
                raise SystemExit(f'patch {k}: want ["the C the instruction gives", "the C to emit instead"]')
            PATCHES[int(k, 16)] = (v[0], v[1])
    extra = []
    for fn in a.extra:
        if os.path.exists(fn):
            for ln in open(fn):
                ln = ln.split('#')[0].strip()
                if ln: extra.append(int(ln.split()[0], 16))
    roots = []
    for fn in a.roots:
        if os.path.exists(fn):
            for ln in open(fn):
                ln = ln.split('#')[0].strip()
                if ln: roots.append(int(ln.split()[0], 16))
    ext = {}
    ext_hdrs = []
    for fn in a.extern:
        mp = os.path.splitext(os.path.basename(fn))[0].replace('_entries', '')
        ext_hdrs.append(f'{mp}_funcs.h')
        for ln in open(fn):
            ext[int(ln.split()[0], 16)] = mp
    exe = Exe(a.input, a.raw)
    for fn in a.scan:
        if os.path.exists(fn):
            d = open(fn, 'rb').read()
            for o in range(0, len(d) - 3, 4):
                v = struct.unpack_from('<I', d, o)[0]
                if exe.inside(v): roots.append(v)
    funcs = discover(exe, extra, roots)
    entries = set(funcs)
    os.makedirs(a.out_dir, exist_ok=True)
    keys = sorted(funcs)
    hdr = [f'/* generated from {os.path.basename(a.input)} */', '#pragma once', '#include "psx.h"']
    for h in ext_hdrs: hdr.append(f'#include "{h}"')
    for e in keys: hdr.append(f'void {P}_{e:08x}(void);')
    open(os.path.join(a.out_dir, f'{P}_funcs.h'), 'w').write(NL.join(hdr) + NL)
    open(os.path.join(a.out_dir, f'{P}_entries.txt'), 'w').write(''.join(f'{e:08x}'+NL for e in keys))
    # unresolved outgoing calls, useful as roots for overlays
    unresolved = set()
    for f in funcs.values():
        for c in f.calls:
            if c not in entries and c not in ext: unresolved.add(c)
    open(os.path.join(a.out_dir, f'{P}_unresolved.txt'), 'w').write(''.join(f'{e:08x}'+NL for e in sorted(unresolved)))
    per = 150
    nfiles = 0
    for n in range(0, len(keys), per):
        out = [f'#include "{P}_funcs.h"', '#if defined(__GNUC__)',
               '#pragma GCC diagnostic ignored "-Wunused-label"',
               '#pragma GCC diagnostic ignored "-Wunused-variable"', '#endif', '']
        for e in keys[n:n + per]:
            n0 = len(out)
            gen_func(exe, funcs[e], entries, out, P, ext)
            if e in a.fullcost:   # timing loops that must not be sped up by the CPU assist
                out[n0:] = [l.replace('  CYC(', '  CYCF(') for l in out[n0:]]
        open(os.path.join(a.out_dir, f'{P}_{nfiles:03d}.c'), 'w').write(NL.join(out))
        nfiles += 1
    tab = [f'#include "{P}_funcs.h"', f'const psx_func_entry {P}_table[] = {{']
    for e in keys: tab.append(f'  {{ 0x{e:08x}u, {P}_{e:08x} }},')
    tab.append('};')
    tab.append(f'const unsigned {P}_table_count = {len(keys)};')
    tab.append(f'const u32 {P}_base = 0x{exe.base:08x}u, {P}_end = 0x{exe.end:08x}u;')
    tab.append(f'const u32 {P}_entry = 0x{(exe.pc0 or 0):08x}u;')
    sig = ', '.join(f'0x{exe.word(exe.base + 4 * k):08x}u' for k in range(8))
    tab.append(f'const u32 {P}_sig[8] = {{ {sig} }};')
    open(os.path.join(a.out_dir, f'{P}_table.c'), 'w').write(NL.join(tab) + NL)
    missing = sorted(set(PATCHES) - PATCHED)
    if missing:   # a patch whose instruction is in no translated function would silently do nothing
        raise SystemExit('patches not applied (not translated as a plain instruction: not reached, a branch or jump, '
                         'or a branch in a delay slot): ' + ', '.join(f'{m:08x}' for m in missing))
    ninsn = sum(len(f.addrs) for f in funcs.values())
    njt = sum(len(f.tables) for f in funcs.values())
    print(f'{P}: {len(keys)} functions, {ninsn} insns, {njt} jump tables, {nfiles} files, {len(unresolved)} unresolved call targets')

if __name__ == '__main__':
    main()
