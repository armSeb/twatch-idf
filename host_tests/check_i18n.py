#!/usr/bin/env python3
"""Vérifie la traduction : chaque _("…") / N_("…") du code a son entrée anglaise
dans main/i18n.c, avec les mêmes spécificateurs printf dans le même ordre."""
import glob, os, re, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "main")
LIT = r'"(?:[^"\\]|\\.)*"'
SEQ = r'((?:\s*' + LIT + r')+)'

def join(seq):
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', seq))

def specs(s):
    return re.findall(r'%[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z)?[diouxXfeEgGcsp%]', s)

src = open(os.path.join(ROOT, "i18n.c"), encoding="utf-8").read()
table = {}
for fr, en in re.findall(r'\{\s*' + SEQ + r'\s*,\s*' + SEQ + r'\s*\}', src):
    table[join(fr)] = join(en)

used, errors = set(), []
for path in sorted(glob.glob(os.path.join(ROOT, "*.c"))):
    if path.endswith("i18n.c"):
        continue
    code = open(path, encoding="utf-8").read()
    for m in re.finditer(r'\bN?_\(' + SEQ + r'\s*\)', code):
        key = join(m.group(1))
        used.add(key)
        line = code.count("\n", 0, m.start()) + 1
        where = f"{os.path.basename(path)}:{line}"
        if key not in table:
            errors.append(f"{where} : pas de traduction pour « {key} »")
        elif specs(key) != specs(table[key]):
            errors.append(f"{where} : formats différents {specs(key)} ≠ {specs(table[key])} pour « {key} »")

for k, v in table.items():
    if specs(k) != specs(v):
        errors.append(f"i18n.c : formats différents pour « {k} »")

unused = sorted(set(table) - used)
for e in errors:
    print("ERREUR", e)
if unused:
    print(f"(info) {len(unused)} entrée(s) inutilisée(s) : " + ", ".join(f"« {u} »" for u in unused))
print(f"I18N : {len(used)} chaînes, {len(table)} traductions" + (" — ÉCHEC" if errors else " — OK"))
sys.exit(1 if errors else 0)
