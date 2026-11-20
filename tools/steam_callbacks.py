#!/usr/bin/env python3
"""Steam callback structs: the guest's (32-bit Windows) layout, from a
Steamworks SDK's headers, for the steam_api bridge's repacking.

Usage: python3 -I tools/steam_callbacks.py SDK/public CLANG OUT.inc

libsteam_api packs callback structs to 4 on macOS and Linux and to 8 on
Windows (VALVE_CALLBACK_PACK_SMALL / _LARGE), so a struct with a 64-bit
field after a 32-bit one has different offsets on each side. clang lays
the headers out for i686-pc-windows-msvc (MSVC's rules); OUT.inc gets,
per struct with a k_iCallback,
  RECOMP_STEAM_CALLBACK(Name, guest_size, F(path, guest_offset), ...)
for each leaf field, and the bridge takes the native offsets and sizes from
the compiler (offsetof, sizeof) on the same headers. A struct with a
pointer field (the in-process HTML surface's) would not survive a copy: it
is left out, with the reason, so delivering one stays a fatal error.
Nothing from the SDK is written elsewhere; the declarations clang needs
from the C library are stubbed in a temporary directory.
"""
import os
import re
import subprocess
import sys
import tempfile

STUBS = {
    'string.h': 'void* memcpy(void*, const void*, size_t); void* memset(void*, int, size_t);\n'
                'int memcmp(const void*, const void*, size_t); size_t strlen(const char*);\n'
                'int strcmp(const char*, const char*); char* strncpy(char*, const char*, size_t);\n',
    'stdio.h': 'int sprintf(char*, const char*, ...); int snprintf(char*, size_t, const char*, ...);\n'
               'int _snprintf(char*, size_t, const char*, ...);\n',
}


def callback_structs(inc):
    names = set()
    for f in headers(inc):
        t = open(os.path.join(inc, 'steam', f), encoding='latin-1').read()
        names |= set(re.findall(r'struct\s+(\w+)\s*\{\s*enum\s*\{\s*k_iCallback\b', t))
        names |= set(re.findall(r'STEAM_CALLBACK_BEGIN\(\s*(\w+)\s*,', t))
    return sorted(names)


def headers(inc):
    # The client interfaces (not the game server's, nor the PS3 renderer's,
    # which needs that platform's headers).
    return sorted(f for f in os.listdir(os.path.join(inc, 'steam'))
                  if f.startswith('isteam') and f.endswith('.h') and
                  'gameserver' not in f and 'ps3' not in f)


def leaves(dump, name):
    """[(path, offset)] of the leaf fields of struct name, and its size."""
    m = re.search(r'^\s*0 \| struct %s(?: \(empty\))?\n(.*?)^\s*\| \[sizeof=(\d+)' % re.escape(name),
                  dump, re.S | re.M)
    if not m:
        sys.exit('steam_callbacks: no layout for %s' % name)
    rows = []
    for line in m.group(1).splitlines():
        lm = re.match(r'^\s*(\d+)(?::\d+-\d+)? \|( +)(.*)$', line)
        if lm:
            rows.append((int(lm.group(1)), len(lm.group(2)), lm.group(3)))
    out, stack = [], []
    skip_below = None  # indentation of a record copied whole
    for i, (off, ind, decl) in enumerate(rows):
        if skip_below is not None:
            if ind > skip_below:
                continue
            skip_below = None
        field = decl.split()[-1]
        while stack and stack[-1][0] >= ind:
            stack.pop()
        stack.append((ind, field))
        deeper = i + 1 < len(rows) and rows[i + 1][1] > ind
        if deeper:
            # A record holding a union (CSteamID, CGameID: pack(1), the same
            # on both sides) is copied whole; others are listed field by field.
            child = rows[i + 1][1]
            j = i + 1
            kids = []
            while j < len(rows) and rows[j][1] > ind:
                if rows[j][1] == child:
                    kids.append(rows[j][2])
                j += 1
            if not any(k.startswith('union') for k in kids):
                continue
            skip_below = ind
        if '*' in decl:
            return None, 'pointer field: %s' % decl
        out.append(('.'.join(f for _, f in stack), off))
    return out, int(m.group(2))


def main():
    inc, clang, out = sys.argv[1], sys.argv[2], sys.argv[3]
    names = callback_structs(inc)
    with tempfile.TemporaryDirectory() as tmp:
        stub = os.path.join(tmp, 'stub')
        os.mkdir(stub)
        for f, body in STUBS.items():
            with open(os.path.join(stub, f), 'w') as h:
                h.write('#pragma once\n#include <stddef.h>\nextern "C" {\n' + body + '}\n')
        tu = os.path.join(tmp, 'tu.cpp')
        with open(tu, 'w') as h:
            h.write('#include "steam/steam_api.h"\n')
            for f in headers(inc):
                h.write('#include "steam/%s"\n' % f)
            for n in names:
                h.write('static_assert(sizeof(%s) > 0, "");\n' % n)
        r = subprocess.run([clang, '-target', 'i686-pc-windows-msvc', '-std=c++17', '-fsyntax-only',
                            '-I' + stub, '-I' + inc, '-Xclang', '-fdump-record-layouts', tu],
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit('steam_callbacks: clang failed:\n' + r.stderr[-3000:])
        dump = r.stdout
    lines = ['// Generated by tools/steam_callbacks.py from the Steamworks SDK headers in',
             '// the build configuration; not part of the source tree.',
             '// With RECOMP_STEAM_HEADERS defined: the headers these structs are in.',
             '#ifdef RECOMP_STEAM_HEADERS']
    lines += ['#include "steam/%s"' % f for f in headers(inc)]
    lines += ['#else']
    skipped = 0
    for n in names:
        fields, size = leaves(dump, n)
        if fields is None:  # left out: delivering one is a fatal error naming it
            lines.append('// %s not translatable (%s)' % (n, size))
            skipped += 1
            continue
        lines.append('RECOMP_STEAM_CALLBACK(%s, %d%s)' % (
            n, size, ''.join(', F(%s, %d)' % (p, o) for p, o in fields)))
    lines.append('#endif')
    with open(out, 'w') as h:
        h.write('\n'.join(lines) + '\n')
    print('steam_callbacks: %d callback structs, %d left out (pointer fields)' %
          (len(names) - skipped, skipped))


if __name__ == '__main__':
    main()
