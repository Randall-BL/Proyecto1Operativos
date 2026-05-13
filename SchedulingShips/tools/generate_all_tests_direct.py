#!/usr/bin/env python3
import os
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
TEST_DIR = os.path.join(ROOT, 'data', 'tests')
os.makedirs(TEST_DIR, exist_ok=True)
algorithms = ['fcfs','sjf','strn','edf','rr','priority']
flows = ['tico','fair','sign']
sides = ['l','r']
types = ['n','p','u']
header_tpl = ('listlen 200\nvisual 100\nflow {flow}\nflowlog on\nw 2\nsignms 8000\nsign r\n'
             'proxpin 22 23\nproxpollms 120\nsensor desactivate\nsensor threshold 10\nreadymax 12\n'
             'alg {alg}\n{rr_line}step n 2\nstep p 2\nstep u 50\n\ndemoclear\n')
count = 0
for alg in algorithms:
    for flow in flows:
        for side in sides:
            for f in types:
                for b in types:
                    fname = f"{alg}_{flow}_{side}_{f}{b}.txt"
                    path = os.path.join(TEST_DIR, fname)
                    rr_line = 'rr 9000\n' if alg == 'rr' else ''
                    content = header_tpl.format(flow=flow, alg=alg, rr_line=rr_line)
                    content += f"demoadd {side} {f} 1\n"
                    content += f"demoadd {side} {b} 1\n"
                    with open(path, 'w', encoding='utf-8') as fh:
                        fh.write(content)
                    count += 1
print('Wrote', count, 'files to', TEST_DIR)
