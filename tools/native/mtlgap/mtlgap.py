#!/usr/bin/env python3
"""Browser Metal gap scan: SDK protocol methods x selectors browsers send x what navi48metal implements.
Usage: mtlgap.py <Metal Headers dir> <Navi48Device.m> <out.json> <name=source dir>...  (run.sh drives it)
"""
import json, os, re, sys
from collections import defaultdict

ID = r'[A-Za-z_]\w*'

def strip_code(t):
    # drop comments and string literals, keep the newlines (line numbers)
    out, i, n = [], 0, len(t)
    while i < n:
        c = t[i]
        if t.startswith('//', i):
            j = t.find('\n', i); i = n if j < 0 else j
        elif t.startswith('/*', i):
            j = t.find('*/', i + 2); j = n if j < 0 else j + 2
            out.append('\n' * t.count('\n', i, j)); i = j
        elif c in '"\'':
            j = i + 1
            while j < n and t[j] != c:
                j += 2 if t[j] == '\\' else 1
            out.append(c + c); i = j + 1
        else:
            out.append(c); i += 1
    return ''.join(out)

# ---------- 1. SDK: protocol -> selectors ----------
def parse_sdk(hdir):
    protos = {}
    for fn in sorted(os.listdir(hdir)):
        if not fn.endswith('.h'):
            continue
        t = strip_code(open(os.path.join(hdir, fn), errors='replace').read())
        for m in re.finditer(r'@protocol\s+(' + ID + r')\b\s*(?:<([^>]*)>)?(?!\s*[;,])(.*?)@end', t, re.S):
            name, parents, body = m.group(1), m.group(2) or '', m.group(3)
            sels = set()
            for mm in re.finditer(r'^\s*[-+]\s*\(.*?;', body, re.S | re.M):
                sels.add(selector_of_decl(mm.group(0)))
            for pm in re.finditer(r'@property\s*(\([^)]*\))?([^;]*);', body):
                attrs = pm.group(1) or ''
                decl = re.sub(r'\b[A-Z][A-Z0-9]*_[A-Z0-9_]*\b\s*(\((?:[^()]|\([^()]*\))*\))?', ' ', pm.group(2))   # drop API_AVAILABLE(...) and friends
                pname = re.findall(ID, decl)[-1]
                g = re.search(r'getter\s*=\s*(' + ID + ')', attrs)
                sels.add(g.group(1) if g else pname)
                if 'readonly' not in attrs:
                    s = re.search(r'setter\s*=\s*(' + ID + ':)', attrs)
                    sels.add(s.group(1) if s else 'set' + pname[0].upper() + pname[1:] + ':')
            protos[name] = {'parents': [p.strip() for p in parents.split(',') if p.strip()],
                            'sels': sorted(s for s in sels if s), 'header': fn}
    return protos

def selector_of_decl(d):
    d = re.sub(r'^\s*[-+]\s*\([^)]*(?:\([^)]*\)[^)]*)*\)', '', d)   # return type
    d = re.sub(r'\([^()]*(?:\([^()]*\)[^()]*)*\)', ' ', d)          # parameter types
    kws = re.findall(r'(' + ID + r')\s*:', d)
    if kws:
        return ''.join(k + ':' for k in kws)
    m = re.match(r'\s*(' + ID + ')', d)
    return m.group(1) if m else None

# ---------- 2. navi48metal: class -> selectors ----------
def parse_ours(path):
    raw = open(path, errors='replace').read()
    t = strip_code(raw)
    impl = defaultdict(set)
    # methods defined in a multi-line #define count for every @implementation that names the macro
    macros = {}
    for m in re.finditer(r'#define\s+(' + ID + r')((?:[^\n]*\\\n)+[^\n]*)', t):
        body = m.group(2).replace('\\\n', '\n')
        sels = {selector_of_decl(x.group(0)) for x in re.finditer(r'^\s*[-+]\s*\(.*?\{', body, re.S | re.M)}
        if sels - {None}:
            macros[m.group(1)] = sels - {None}
    for m in re.finditer(r'@implementation\s+(' + ID + r')(?:\s*\([^)]*\))?(.*?)@end', t, re.S):
        body = re.sub(r'#define[^\n]*(?:\\\n[^\n]*)*', '', m.group(2))
        for mm in re.finditer(r'^\s*[-+]\s*\(.*?\{', body, re.S | re.M):
            s = selector_of_decl(mm.group(0))
            if s:
                impl[m.group(1)].add(s)
        for name, sels in macros.items():
            if re.search(r'\b' + name + r'\b', body):
                impl[m.group(1)] |= sels
    pairs = re.findall(r'\{\s*\[?(' + ID + r')(?:\s+class\])?\s*,\s*"(' + ID + r')"\s*,\s*"(' + ID + r')"\s*\}', raw)
    cls2proto = {name: proto for _, name, proto in pairs}
    return {k: sorted(v) for k, v in impl.items()}, cls2proto

# ---------- 3. browser sources: the selectors they send ----------
def sends_in(text):
    """[(selector, offset, kind)], kind = msg / get (dot syntax) / set (dot assignment)"""
    res, stack = [], []
    for i, c in enumerate(text):
        if c == '[':
            stack.append(i)
        elif c == ']' and stack:
            s = stack.pop()
            body = flatten(text[s + 1:i])
            sel = msg_selector(body)
            if sel:
                res.append((sel, s, 'msg'))
    for m in re.finditer(r'(?<=[\w)\]])\.(' + ID + r')\b(?!\s*\()(\s*=(?!=))?', text):
        name = m.group(1)
        if m.group(2):
            res.append(('set' + name[0].upper() + name[1:] + ':', m.start(), 'set'))
        else:
            res.append((name, m.start(), 'get'))
    return res

def flatten(b):
    # collapse nested () [] {} to a placeholder, keep the top level
    out, d = [], 0
    for c in b:
        if c in '([{':
            d += 1
            if d == 1:
                out.append('x')
        elif c in ')]}':
            d = max(0, d - 1)
        elif d == 0:
            out.append(c)
    return ''.join(out)

def msg_selector(b):
    kws = list(re.finditer(r'(?<![\w:])(' + ID + r'):(?!:)', b))
    if kws:
        recv = b[:kws[0].start()].strip()
        if not recv or recv[-1] in '?:,=(&|!<>+-*/%' or recv in ('return', 'case'):
            return None
        return ''.join(k.group(1) + ':' for k in kws)
    m = re.fullmatch(r'\s*(\S.*?)\s+(' + ID + r')\s*', b, re.S)
    if m and m.group(1)[-1] not in '+-*/%=<>&|!?:,' and not re.fullmatch(r'(new|delete|return|const|static|int|auto)', m.group(1)):
        return m.group(2)
    return None

def scan_project(root):
    hits = defaultdict(list)          # sel -> [(relpath, line, kind)]
    for dp, _, fns in os.walk(root):
        if '/.git' in dp:
            continue
        for fn in fns:
            if not fn.endswith(('.m', '.mm', '.h')):
                continue
            p = os.path.join(dp, fn)
            try:
                raw = open(p, errors='replace').read()
            except OSError:
                continue
            if 'MTL' not in raw:      # Metal files only: fewer same-named non-Metal selectors
                continue
            t = strip_code(raw)
            rel = os.path.relpath(p, root)
            for sel, off, kind in sends_in(t):
                hits[sel].append((rel, t.count('\n', 0, off) + 1, kind))
    return hits

def main():
    hdir, ours_path, out = sys.argv[1:4]
    protos = parse_sdk(hdir)
    impl, cls2proto = parse_ours(ours_path)
    projects = {}
    for arg in sys.argv[4:]:
        name, root = arg.split('=', 1)
        projects[name] = scan_project(root)
    json.dump({'protos': protos, 'impl': impl, 'cls2proto': cls2proto,
               'hits': {n: {s: v for s, v in h.items()} for n, h in projects.items()}},
              open(out, 'w'))
    print('protocols', len(protos), 'our classes', len(impl), 'pairs', len(cls2proto),
          {n: len(h) for n, h in projects.items()})

if __name__ == '__main__':
    main()
