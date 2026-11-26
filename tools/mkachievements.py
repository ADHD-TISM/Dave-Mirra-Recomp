"""Write the Recomp Achievements lists, achievements/dmbmx.json and achievements/maxremix.json.

Memory rules (rcheevos conditions) on PlayStation RAM, found with --ramdump/--poke runs and checked on the rider
card and the level select (see the commit that adds this file). Both games keep ProQuest in rider records:
  record + 0x0c + 39*L   level L's tier: 0 none, 1 amateur, 2 pro, 3 hardcore challenges done
                          (each level block starts a byte before it; after it come a byte, then 12 challenges of
                          3 bytes; bit 7 of a challenge's first byte = done)
  record + MEDALS + i    Pro Tour competition i: 0 none, 1 bronze, 2 silver, 3 gold
The records stay put during a level; the "selected rider" byte does not (it is reused in a level), so every rule
looks at every record. A value above 3 never fires a rule. Boot is safe because RAM starts zeroed and rcheevos waits
for a rule to be false for a frame; a rule can fire when a save with progress loads, which the game reports.
The HUD score is a 32-bit word (checked by writing a number there and seeing it on the HUD).
usage: python tools/mkachievements.py   (from the project folder)"""
import json
import os

GAMES = {
    "dmbmx": dict(game="dave-mirra-freestyle-bmx", serial="SLUS-01026", base=0x0B6014, stride=0x104, riders=12,
                  levels=["Greenville - Vail Trails", "Greenville - Greenville Park", "Greenville - Backyard Pools",
                          "Woodward - Woodward Trails", "Woodward - Lot 8", "Woodward - Titanic"],
                  medals=0xF5, comps=6, score=0x08A050),
    # 28 records: 14 riders, the second 14 presumably the Expert difficulty that Maximum Remix adds
    "maxremix": dict(game="dave-mirra-freestyle-bmx-maximum-remix", serial="SLUS-01347", base=0x0832F0, stride=0x260,
                     riders=28, levels=[None] * 15, medals=0x254, comps=5, score=0x0911B0),
}
NUM = {6: "six", 5: "five", 15: "fifteen"}


def h(addr):
    return "0xH%06x" % addr


def rng(addr, cmp):
    """addr compared as cmp, and never above 3 (a real tier or medal); =3 needs no upper bound."""
    if cmp in ("=3", ">=3"):
        return "%s=3" % h(addr)
    return "%s%s_%s<=3" % (h(addr), cmp, h(addr))


def make(g):
    tier = lambda r, L: g["base"] + r * g["stride"] + 0x0C + 39 * L
    medal = lambda r, i: g["base"] + r * g["stride"] + g["medals"] + i
    R, NL, NC = range(g["riders"]), len(g["levels"]), g["comps"]

    def any_tier(cmp):
        return "".join("S" + rng(tier(r, L), cmp) for r in R for L in range(NL))

    def any_medal(cmp):
        return "".join("S" + rng(medal(r, i), cmp) for r in R for i in range(NC))

    def one_rider_all(addr_of, n, cmp):
        return "".join("S" + "_".join(rng(addr_of(r, k), cmp) for k in range(n)) for r in R)

    def level_any_rider(L, cmp):
        return "".join("S" + rng(tier(r, L), cmp) for r in R)

    nl, nc = NUM[NL], NUM[NC]
    a = [("Amateur Hour", "ProQuest: finish all four amateur challenges in a level", 5, "progression", any_tier(">=1")),
         ("Going Pro", "ProQuest: finish all four pro challenges in a level", 10, "progression", any_tier(">=2")),
         ("Hardcore", "ProQuest: finish all four hardcore challenges in a level", 25, None, any_tier(">=3"))]
    for L, name in enumerate(g["levels"]):
        if name:
            a.append(("%s Mastered" % name.split(" - ")[1], "ProQuest: finish every challenge in %s" % name, 10, None,
                      level_any_rider(L, ">=3")))
    a += [("Amateur Career", "ProQuest: finish the amateur challenges in all %s levels with one rider" % nl, 10, "progression",
           one_rider_all(tier, NL, ">=1")),
          ("Pro Career", "ProQuest: finish the pro challenges in all %s levels with one rider" % nl, 25, "progression",
           one_rider_all(tier, NL, ">=2")),
          ("Legend", "ProQuest: finish every challenge in all %s levels with one rider" % nl, 50, "win",
           one_rider_all(tier, NL, ">=3")),
          ("On the Podium", "ProQuest: win a medal in a Pro Tour competition", 10, None, any_medal(">=1")),
          ("Gold Standard", "ProQuest: win a Pro Tour competition", 10, None, any_medal("=3")),
          ("Tour Champion", "ProQuest: win gold in all %s Pro Tour competitions with one rider" % nc, 50, "win",
           one_rider_all(medal, NC, "=3"))]
    s = g["score"]
    for t, d, p, v in (("Five Figures", "Score 10,000 points in one run", 5, 10000),
                       ("Big Numbers", "Score 50,000 points in one run", 10, 50000),
                       ("Six Figures", "Score 100,000 points in one run", 25, 100000)):
        a.append((t, d, p, None, "0xX%06x>=%d_0xX%06x<=9999999" % (s, v, s)))
    out = {"format": 1, "game": g["game"], "source": "memory", "console": "psx", "copies": [g["serial"]],
           "versions": ">=1.2.3", "achievements": []}   # v1.2.3 is the first with the achievements runtime
    for i, (t, d, p, ty, tr) in enumerate(a, 1):
        e = {"id": i, "title": t, "description": d, "points": p}
        if ty:
            e["type"] = ty
        e["trigger"] = tr
        out["achievements"].append(e)
    return out


here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "achievements")
os.makedirs(here, exist_ok=True)
for gid, g in GAMES.items():
    doc = make(g)
    with open(os.path.join(here, gid + ".json"), "w", newline="\n") as f:
        f.write(json.dumps(doc, indent=2) + "\n")
    print(gid, len(doc["achievements"]), "achievements")
