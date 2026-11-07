#!/usr/bin/env python3
"""Checks that every string passed to _() / N_() has a Turkish translation in one of the
*.tr.inc tables and that printf specifiers match between English and Turkish.

usage: check_i18n.py [SRC_DIR] [--list]   (--list prints missing keys as .tr.inc lines)"""
import os
import re
import sys

STR = r'"((?:[^"\\\n]|\\.)*)"'
CALL = re.compile(r'\bN?_\(\s*' + STR + r'\s*\)')
ENTRY = re.compile(r'\{\s*' + STR + r'\s*,\s*' + STR + r'\s*\}')
FMT = re.compile(r'%[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z)?[diouxXfFeEgGcsp%]')


def sources(root):
    for d, _, files in os.walk(root):
        for f in files:
            if f.endswith('.c'):
                yield os.path.join(d, f)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    root = args[0] if args else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src')
    table = {}
    for d, _, files in os.walk(root):
        for f in files:
            if f.endswith('tr.inc'):
                with open(os.path.join(d, f), encoding='utf-8') as fh:
                    for en, tr in ENTRY.findall(fh.read()):
                        table.setdefault(en, tr)
    missing, bad = {}, []
    for path in sources(root):
        with open(path, encoding='utf-8') as fh:
            text = fh.read()
        for m in CALL.finditer(text):
            key = m.group(1)
            if key not in table:
                missing.setdefault(key, os.path.relpath(path, root))
            elif [f for f in FMT.findall(key) if f != '%%'] != [f for f in FMT.findall(table[key]) if f != '%%']:
                bad.append((key, table[key]))
    for key, tr in bad:
        print(f'format mismatch: "{key}" -> "{tr}"')
    if '--list' in sys.argv:
        for key, where in sorted(missing.items(), key=lambda kv: kv[1]):
            print(f'\t{{"{key}", ""}},  /* {where} */')
    else:
        for key, where in sorted(missing.items(), key=lambda kv: kv[1]):
            print(f'missing translation ({where}): "{key}"')
    print(f'{len(table)} translations, {len(missing)} missing, {len(bad)} format mismatches')
    return 1 if missing or bad else 0


if __name__ == '__main__':
    sys.exit(main())
