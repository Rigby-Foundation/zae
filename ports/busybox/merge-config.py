#!/usr/bin/env python3
# merge-config.py .config want: set each CONFIG_x of `want` (=y, =value, or =n) in a BusyBox .config
import re, sys
cfg, want = sys.argv[1], sys.argv[2]
w = {}
for l in open(want):
    l = l.strip()
    if l and not l.startswith('#'):
        k, v = l.split('=', 1)
        if not v.startswith('"'):
            v = v.split('#', 1)[0].strip()      # a comment after the value
        w[k] = v
out, seen = [], set()
for l in open(cfg).read().split('\n'):
    m = re.match(r'^(?:# )?(CONFIG_\w+)(?:=| is not set)', l)
    if m and m.group(1) in w:
        k = m.group(1); seen.add(k)
        out.append(f'# {k} is not set' if w[k] == 'n' else f'{k}={w[k]}')
    else:
        out.append(l)
out += [f'# {k} is not set' if v == 'n' else f'{k}={v}' for k, v in w.items() if k not in seen]
open(cfg, 'w').write('\n'.join(out))
