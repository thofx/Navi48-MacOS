#!/usr/bin/env python3
"""gap.json -> gap list.  Usage: report.py gap.json out.md out.tsv Navi48Device.m"""
import json, re, sys
from collections import defaultdict
from typing import NamedTuple

scan = json.load(open(sys.argv[1]))
protos, impl, cls2proto, hits = scan['protos'], scan['impl'], scan['cls2proto'], scan['hits']
PROJECTS = list(hits)

# our classes' superclasses (Navi48Device.m @interface): NSObject = the static answer is final, an Apple base class may still implement the selector (census)
SUPER = dict(re.findall(r'@interface\s+(\w+)\s*:\s*(\w+)', open(sys.argv[4]).read()))


class Row(NamedTuple):
    kind: str           # missing-method (one of our classes lacks it) / no-class (no class of ours for the protocol)
    protocol: str
    our_class: str      # Class:Superclass:certain|census?  or '-'
    selector: str
    core_sites: int     # call sites outside WebGPU paths
    n_projects: int
    n_sites: int
    send_kind: str      # msg / get / set
    projects: str
    sample: str         # project:file:line


def tier(project, path):
    # WebGPU paths rank last; everything else is a main rendering path (ANGLE / Skia / compositing)
    return 'webgpu' if project == 'dawn' or '/WebGPU/' in path else 'core'


def all_sels(proto, seen=None):
    seen = seen if seen is not None else set()
    if proto not in protos or proto in seen:
        return set()
    seen.add(proto)
    sels = set(protos[proto]['sels'])
    for parent in protos[proto]['parents']:
        sels |= all_sels(parent, seen)
    return sels


def call_sites(sel):
    return {project: hits[project][sel] for project in PROJECTS if hits[project].get(sel)}


rows = []
proto2cls = {v: k for k, v in cls2proto.items()}
for proto in sorted(protos):
    if proto.startswith('MTL4'):
        continue        # ponytail: Metal 4 left out, browsers do not use it yet; add it when they do
    cls = proto2cls.get(proto)
    have = set(impl.get(cls, [])) if cls else set()
    for sel in sorted(all_sels(proto)):
        sites = call_sites(sel)
        if not sites or sel in have:
            continue
        first_project, first_sites = next(iter(sites.items()))
        sup = SUPER.get(cls, '')
        rows.append(Row(
            kind='missing-method' if cls else 'no-class', protocol=proto,
            our_class=f"{cls}:{sup}:{'certain' if sup == 'NSObject' else 'census?'}" if cls else '-', selector=sel,
            core_sites=sum(1 for project, v in sites.items() for path, _, _ in v if tier(project, path) == 'core'),
            n_projects=len(sites), n_sites=sum(len(v) for v in sites.values()),
            send_kind='/'.join(sorted({k for v in sites.values() for _, _, k in v})), projects=','.join(sorted(sites)),
            sample=f'{first_project}:{first_sites[0][0]}:{first_sites[0][1]}'))

rows.sort(key=lambda r: (r.kind, -r.core_sites, -r.n_projects, -r.n_sites))
with open(sys.argv[3], 'w') as f:
    f.write('\t'.join(Row._fields) + '\n')
    for r in rows:
        f.write('\t'.join(map(str, r)) + '\n')

# grouped by protocol
groups = defaultdict(list)
for r in rows:
    groups[(r.kind, r.protocol, r.our_class)].append(r)
with open(sys.argv[2], 'w') as f:
    for (kind, proto, cls), rs in sorted(groups.items(), key=lambda kv: (kv[0][0], -sum(r.core_sites for r in kv[1]))):
        f.write(f'\n## {proto} ({cls}) — {kind}, {len(rs)} selectors\n')
        for r in rs:
            f.write(f'- `{r.selector}` core={r.core_sites} projects={r.n_projects} sites={r.n_sites} [{r.send_kind}] {r.projects}  e.g. {r.sample}\n')
print(len(rows), 'rows;', sum(1 for r in rows if r.kind == 'missing-method'), 'missing-method;',
      len({r.protocol for r in rows if r.kind == 'no-class'}), 'protocols without a class')
