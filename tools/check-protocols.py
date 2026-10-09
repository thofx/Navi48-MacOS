#!/usr/bin/env python3
"""check-protocols.py - compile-time check of navi48metal's classes against the Metal protocols they claim at run time.

The bundle adds the MTL* protocols with class_addProtocol in +load, so the compiler never sees them: a misspelt selector or a wrong
return type compiles clean and fails only on the PC. This check compiles a scratch copy of Navi48Device.m in which every class of the
census table ({ [Cls class], "Cls", "MTLProto" }) declares its protocol, and reads the compiler's verdicts:
  * a signature that differs from the SDK in a way the ABI sees (BOOL vs NSUInteger, float vs double, ...) fails. Differences the ABI
    does not see (NSUInteger vs an NS_ENUM(NSUInteger), id vs id<MTLBuffer>, nullability) are accepted: the code base spells them
    that way on purpose;
  * the required methods a class does not implement (the Apple base class serves them at run time, see n48_census.h) must equal the
    committed baseline. A misspelt selector shows up as the real one going missing; a newly implemented method as a stale line.
Usage: check-protocols.py <Navi48Device.m> <baseline.txt> <Metal Headers dir> [--update] -- <compiler argv (no input / -c / -o)>
"""
import os, re, subprocess, sys, tempfile

def enum_types(hdir: str) -> dict[str, str]:
    m: dict[str, str] = {}
    for fn in os.listdir(hdir):
        if fn.endswith('.h'):
            for base, name in re.findall(r'\bNS_(?:ENUM|OPTIONS|CLOSED_ENUM)\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)', open(os.path.join(hdir, fn), errors='replace').read()):
                m[name] = base
    return m

PLAIN = {'NSUInteger': 'unsigned long', 'NSInteger': 'long', 'BOOL': 'signed char', 'CGFloat': 'double'}

def abi_type(t: str, enums: dict[str, str]) -> str:
    # What the x86_64 ABI sees: every object, pointer and block is one pointer register ('ptr'); an NS_ENUM is its base integer type.
    #   'MTLPurgeableState' (aka 'enum MTLPurgeableState') -> 'unsigned long';  'id<MTLBuffer> _Nullable' -> 'ptr'
    t = re.sub(r'\b(_Nonnull|_Nullable|_Nullable_result|_Null_unspecified|__unsafe_unretained|__strong|__weak|__autoreleasing|const|volatile)\b', ' ', t)
    t = re.sub(r'\benum\s+', '', t).strip()
    if '*' in t or '^' in t or re.fullmatch(r'(id|Class|SEL)\b.*', t):
        return 'ptr'
    def resolve(m: re.Match[str]) -> str:
        base = enums.get(m.group(1), m.group(1))
        return PLAIN.get(base, base)
    return re.sub(r'\s+', ' ', re.sub(r'\b(\w+)\b', resolve, t))

def canon(spelled, aka):
    return aka if aka else spelled

def main():
    argv = sys.argv[1:]
    sep = argv.index('--')
    opts, cc = argv[:sep], argv[sep + 1:]
    update = '--update' in opts
    src, baseline, hdir = [o for o in opts if o != '--update']
    enums = enum_types(hdir)
    text = open(src).read()
    pairs = re.findall(r'\{\s*\[?\w+(?:\s+class\])?\s*,\s*"(\w+)"\s*,\s*"(\w+)"\s*\}', text)
    if not pairs:
        sys.exit('check-protocols: no census table found in ' + src)
    for cls, proto in pairs:
        text, n = re.subn(r'^(@interface\s+' + cls + r'\s*:\s*\w+)', r'\1 <' + proto + '>', text, count=1, flags=re.M)
        if n != 1:
            sys.exit(f'check-protocols: no @interface for {cls}')
    with tempfile.TemporaryDirectory() as tmp:
        copy = os.path.join(tmp, os.path.basename(src))
        open(copy, 'w').write(text)
        env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=os.path.join(tmp, 'zg'), ZIG_LOCAL_CACHE_DIR=os.path.join(tmp, 'zl'))   # zig replays no warnings on a cache hit
        r = subprocess.run(cc + ['-Wprotocol', '-Wno-error', '-c', copy, '-o', os.path.join(tmp, 'x.o')], capture_output=True, text=True, env=env)
    impl_at = [(i + 1, m.group(1)) for i, line in enumerate(text.split('\n')) for m in [re.match(r'@implementation\s+(\w+)', line)] if m]
    def cls_at(line):
        return ([c for l, c in impl_at if l <= line] or ['?'])[-1]
    missing, bad, errors = set(), [], []
    for line in r.stderr.split('\n'):
        w = re.match(r'.*?:(\d+):\d+: (warning|error): (.*)$', line)
        if not w:
            continue
        ln, kind, msg = int(w.group(1)), w.group(2), w.group(3)
        if kind == 'error':
            errors.append(line)
        elif (m := re.match(r"method '([^']+)' in protocol '(\w+)' not implemented", msg)):
            missing.add(f'{cls_at(ln)} {m.group(1)}')
        elif (m := re.match(r"auto property synthesis will not synthesize property '(\w+)' declared in protocol '(\w+)'", msg)):
            missing.add(f'{cls_at(ln)} {m.group(1)}')
        elif (m := re.match(r"conflicting (?:return|parameter) types? in implementation of '([^']+)': '([^']+)'(?: \(aka '([^']+)'\))? vs '([^']+)'(?: \(aka '([^']+)'\))?", msg)):
            a, b = abi_type(canon(m.group(2), m.group(3)), enums), abi_type(canon(m.group(4), m.group(5)), enums)
            if a != b:
                bad.append(f'{cls_at(ln)} {m.group(1)}: the SDK says {m.group(2)}, the bundle {m.group(4)} ({a} vs {b}) [line {ln} of the copy]')
    if r.returncode != 0 and not errors:
        errors.append(f'compiler exited {r.returncode}: {r.stderr.strip()[-400:]}')
    if update:
        open(baseline, 'w').write('# Required MTL* protocol methods each navi48metal class leaves to its Apple base class (served at run time; see n48_census.h).\n'
                                  '# Generated by tools/check-protocols.py --update (via tools/check.sh --update-protocols); a diff here is a decision, review it.\n'
                                  + ''.join(f'{x}\n' for x in sorted(missing)))
    want = set(x.strip() for x in open(baseline) if x.strip() and not x.startswith('#')) if os.path.exists(baseline) else set()
    gone, new = sorted(want - missing), sorted(missing - want)
    for e in errors:
        print('ERROR', e)
    for b in bad:
        print('ABI MISMATCH', b)
    for x in new:
        print('NOT IMPLEMENTED (new)', x, '- a misspelt or removed method? If intended: --update')
    for x in gone:
        print('NOW IMPLEMENTED', x, '- remove it from the baseline (--update)')
    print(f'check-protocols: {len(pairs)} classes, {len(missing)} required methods left to the base classes, '
          f'{len(bad)} ABI mismatch(es), {len(new)} new / {len(gone)} stale baseline line(s)')
    sys.exit(1 if errors or bad or new or gone else 0)

if __name__ == '__main__':
    main()
