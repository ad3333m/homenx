# Decodes Atmosphère cheat codes (for tools/make_*_packs.py) into the writes they make: (bytes, address, value), with the
# address in HomeNX pack syntax (main+X, [..] pointer chains). Only the plain shapes: static
# writes (type 0), pointer chains (5 / 7 / 6); conditions (1 / 2) are passed over, anything
# needing a button (8) or loops / register maths (3, 4, 9, A, C, F) makes the cheat unusable.
import json, os, re, sys

REGIONS = ["main", "heap", "alias", "aslr"]


def decode(text):
    regs, writes, keyed = {}, [], False
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("[") or line.startswith("{"):
            continue
        t = line.split()
        try:
            first = t[0].upper()
            k = first[0]
            if k == "0":  # 0TMR00AA AAAAAAAA VVVVVVVV (VVVVVVVV)
                width, region = int(first[1], 16), int(first[2], 16)
                # (its offset register adds 0 unless this cheat set it: many codes name F without using it)
                if (first[3] != "0" and first[3] in regs) or region > 2:
                    return None, keyed
                addr = int(first[6:8] + t[1], 16)
                value = int("".join(t[2:4]) if width == 8 else t[2], 16)
                writes.append((width, f"{REGIONS[region]}+{addr:X}", value))
            elif k == "5":  # 5TMRI0AA AAAAAAAA: rR = [region+A] or [rR+A]
                region, reg, fromReg = int(first[2], 16), first[3], first[4]
                addr = int(first[6:8] + t[1], 16)
                if fromReg == "0":
                    if region > 2:
                        return None, keyed
                    regs[reg] = f"[{REGIONS[region]}+{addr:X}]"
                elif reg in regs:
                    regs[reg] = f"[{regs[reg]}+{addr:X}]"
                else:
                    return None, keyed
            elif k == "7":  # 7T0RC000 VVVVVVVV: rR += / -= V
                reg, op, v = first[3], first[4], int(t[1], 16)
                if reg not in regs or op not in "01":
                    return None, keyed
                regs[reg] = f"{regs[reg]}{'+' if op == '0' else '-'}{v:X}"
            elif k == "6":  # 6T0RIor0 VVVVVVVV VVVVVVVV: [rR] = V
                width, reg, offReg = int(first[1], 16), first[3], first[5]
                if reg not in regs or offReg != "0":
                    return None, keyed
                writes.append((width, regs[reg], int(t[1] + t[2], 16)))
            elif k in "12":  # conditions: their writes still say where the value is
                continue
            elif k == "8":  # needs a button: not a counter
                keyed = True
                return None, keyed
            else:
                return None, keyed
        except (IndexError, ValueError):
            return None, keyed
    return writes, keyed


def slotless(name):  # "[9999 Coins Save 1]" -> "9999 Coins"
    n = name.strip("[] ")
    n = re.sub(r"\s*[\(\[]?\s*(save|slot|file)\s*\d+\s*[\)\]]?", "", n, flags=re.I)
    return n.strip(" -:")
