#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026-2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import csv
import re
import subprocess
from pathlib import Path

import vltest_bootstrap
from repcut_common import partitioner

test.scenarios('vltmt')
test.top_filename = str(Path(test.top_filename).resolve())
base = Path(test.obj_dir) / (test.vm_prefix + '__repcut_early')
for case, threads in [('stock', 1), ('disabled', 2), ('default', 2), ('source', 2), ('serial', 1),
                      ('fallback', 2), ('protected', 2)]:
    flags = [
        '--stats', '--no-skip-identical', '-fno-table', '--debug-check', '-CFLAGS',
        f'-DREPCUT_TEST_THREADS={threads}'
    ]
    if case == 'fallback':
        flags += ['--output-split-cfuncs', '512']
    if case == 'protected':
        flags += ['--protect-ids', '--protect-key', 'REPCUT_PRIORITY']
    runtime = []
    if case != 'stock':
        if case == 'disabled':
            flags += ['--no-repcut-priority-tables']
        elif case == 'source':
            flags += ['--repcut-priority-tables']
        test.compile(make_main=False,
                     verilator_make_gmake=False,
                     threads=threads,
                     verilator_flags2=flags + ['--repcut-dump-graph'])
        with base.with_suffix('.nodes.tsv').open(encoding="utf8") as stream:
            nodes = list(csv.DictReader(stream, delimiter='\t'))
        dependencies = [[int(x) for x in node['inputs'].split(',') if x] for node in nodes]
        with base.with_suffix('.controls.tsv').open(encoding="utf8") as stream:
            for row in csv.DictReader(stream, delimiter='\t'):
                dependencies[int(row['node'])].append(int(row['predicate']))
        parts = [set() for _ in range(threads)]
        sinks = [int(n['id']) for n in nodes if n['kind'] in ('NEXT', 'OUTPUT', 'EFFECT')]
        for ordinal, sink in enumerate(sinks):
            pending = [sink]
            part = parts[ordinal % threads]
            while pending:
                node = pending.pop()
                if node not in part:
                    part.add(node)
                    pending.extend(dependencies[node])
        membership = Path(test.obj_dir) / 'membership.csv'
        membership.write_text(''.join(','.join(map(str, sorted(p))) + '\n' for p in parts),
                              encoding="utf8")
        runtime = ['--repcut', '--repcut-partitioner', partitioner(test, membership)]
    test.compile(make_main=False,
                 verilator_make_gmake=False,
                 threads=threads,
                 verilator_flags2=flags + runtime +
                 ['--exe', str(Path(test.pli_filename).resolve())])
    stats = Path(test.stats).read_text(encoding="utf8")
    (Path(test.obj_dir) / (case + '.stats')).write_text(stats, encoding="utf8")
    if case not in ('stock', 'disabled'):
        count = re.search(r'RepCut priority, chains lowered\s+(\d+)', stats)
        assert count and int(count[1]) >= 32, case
        if case == 'fallback':
            count = re.search(r'RepCut native source, (?:graph fallback|hybrid) programs\s+(\d+)',
                              stats)
            assert count and int(count[1]) > 0
    else:
        assert 'RepCut priority, chains lowered' not in stats
    subprocess.run(['make', '-C', test.obj_dir, '-f', test.vm_prefix + '.mk', '-j2'], check=True)
    result = subprocess.run([str(Path(test.obj_dir).resolve() / test.vm_prefix)],
                            capture_output=True,
                            text=True,
                            timeout=60,
                            check=False)
    (Path(test.obj_dir) / (case + '.log')).write_text(result.stdout + result.stderr,
                                                      encoding="utf8")
    result.check_returncode()
    assert '*-* All Finished *-*' in result.stdout
test.passes()
