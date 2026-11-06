#!/usr/bin/env python3
"""Lists the interface texts the code translates, and keeps lang/<code>.txt in step with them.

A text is a key when it is a string literal (or adjacent literals) written straight inside tr(...), trId(...)
or N_(...) in src/gui; trc("context", "English") gives the key "context|English". The part after ## / ### (an ImGui id) is not part of the key.

  tr_keys.py --list                     every key, with where it is used
  tr_keys.py --check lang/ru.txt        fails (exit 1) when a key used in the code is not in the catalogue
  tr_keys.py --update lang/ru.txt       rewrites the catalogue in code order: new keys added with tr "",
                                        translations kept, keys the code no longer uses moved to the end
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
SRC = os.path.join(ROOT, 'src', 'gui')
MARKERS = ('tr', 'trId', 'N_')

ESC = {'n': '\n', 't': '\t', '"': '"', '\\': '\\', "'": "'", 'r': '\r', '0': '\0', '?': '?', 'a': '\a'}


def decode(raw):
    out, i = [], 0
    while i < len(raw):
        c = raw[i]
        if c == '\\' and i + 1 < len(raw):
            n = raw[i + 1]
            if n == 'x':
                j = i + 2
                while j < len(raw) and raw[j] in '0123456789abcdefABCDEF':
                    j += 1
                out.append(chr(int(raw[i + 2:j], 16)))
                i = j
                continue
            out.append(ESC.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def keys_in(path):
    """(key, line, source line) for each literal written directly inside tr()/trId()/N_()"""
    src = open(path, encoding='utf-8').read()
    lines = src.split('\n')
    res = []
    for m in re.finditer(r'(?<![A-Za-z0-9_:.>])(tr|trId|N_|trc)\(\s*(?=")', src):
        i = m.end()
        ctx = None
        if m.group(1) == 'trc':  # trc("context", "English"): the key is "context|English"
            mc = re.match(r'"([A-Za-z0-9_ ]+)"\s*,\s*(?=")', src[i:])
            if not mc:
                continue
            ctx = mc.group(1)
            i += mc.end()
        parts = []
        # adjacent literals, with comments and whitespace between them
        while True:
            while i < len(src) and src[i] in ' \t\r\n':
                i += 1
            if src.startswith('//', i):
                i = src.index('\n', i)
                continue
            if i < len(src) and src[i] == '"':
                j = i + 1
                while src[j] != '"':
                    j += 2 if src[j] == '\\' else 1
                parts.append(src[i + 1:j])
                i = j + 1
                continue
            break
        if not parts or i >= len(src) or src[i] != ')':
            continue  # tr("a" + b): not a plain literal
        text = decode(''.join(parts))
        for sep in ('###', '##'):
            if sep in text:
                text = text.split(sep)[0]
                break
        if ctx:
            text = ctx + '|' + text
        if text:
            ln = src.count('\n', 0, m.start()) + 1
            res.append((text, ln, lines[ln - 1].strip()))
    return res


def all_keys():
    """key -> list of 'file:line  code'"""
    keys = {}
    for name in sorted(os.listdir(SRC)):
        if name.endswith(('.cpp', '.h')):
            for text, ln, code in keys_in(os.path.join(SRC, name)):
                keys.setdefault(text, []).append('%s:%d  %s' % (name, ln, code[:150]))
    return keys


def quote(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n').replace('\t', '\\t').replace('\r', '\\r') + '"'


def unquote(s, lineno, path):
    s = s.strip()
    if len(s) < 2 or s[0] != '"' or s[-1] != '"':
        raise SystemExit('%s:%d: a text in "quotes" was expected' % (path, lineno))
    body, out, i = s[1:-1], [], 0
    while i < len(body):
        c = body[i]
        if c == '\\' and i + 1 < len(body):
            out.append({'n': '\n', 't': '\t', 'r': '\r', '"': '"', '\\': '\\', '0': '\0'}.get(body[i + 1], body[i + 1]))
            i += 2
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def read_catalogue(path):
    """(header lines, {en: tr}) ; tr '' = not translated"""
    header, entries, cur = [], {}, None
    if not os.path.exists(path):
        return header, entries
    seen_entry = False
    for n, line in enumerate(open(path, encoding='utf-8-sig'), 1):
        s = line.rstrip('\r\n')
        t = s.strip()
        if t.startswith('en '):
            cur = unquote(t[3:], n, path)
            entries.setdefault(cur, '')
            seen_entry = True
        elif t.startswith('tr '):
            if cur is None:
                raise SystemExit('%s:%d: tr without en' % (path, n))
            entries[cur] = unquote(t[3:], n, path)
            cur = None
        elif not seen_entry and (t.startswith('language ') or t.startswith('#') or not t):
            header.append(s)
    return header, entries


def main():
    args = sys.argv[1:]
    keys = all_keys()
    if not args or args[0] == '--list':
        for k, where in keys.items():
            print(quote(k), ' <- ', where[0])
        print('%d keys' % len(keys), file=sys.stderr)
        return 0
    if args[0] == '--check' and len(args) == 2:
        _, cat = read_catalogue(args[1])
        missing = [k for k in keys if k not in cat]
        untranslated = [k for k in keys if k in cat and not cat[k]]
        for k in missing:
            print('not in %s: %s  (%s)' % (os.path.basename(args[1]), quote(k)[:120], keys[k][0].split('  ')[0]))
        print('%d keys used in the code, %d in the catalogue, %d missing, %d not translated yet' %
              (len(keys), len(cat), len(missing), len(untranslated)))
        return 1 if missing else 0
    if args[0] == '--update' and len(args) == 2:
        path = args[1]
        header, cat = read_catalogue(path)
        if not header:
            header = ['language ""', '']
        out = list(header)
        if out and out[-1].strip():
            out.append('')
        for k, where in keys.items():
            for w in where[:3]:
                out.append('# ' + w)
            out.append('en ' + quote(k))
            out.append('tr ' + quote(cat.get(k, '')))
            out.append('')
        old = [k for k in cat if k not in keys]
        if old:
            out.append('# ---- no longer used by the code (kept for reference; safe to delete) ----')
            out.append('')
            for k in old:
                out.append('en ' + quote(k))
                out.append('tr ' + quote(cat[k]))
                out.append('')
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(out).rstrip('\n') + '\n')
        print('%s: %d keys, %d translated, %d no longer used' % (path, len(keys), sum(1 for k in keys if cat.get(k)), len(old)))
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main())
