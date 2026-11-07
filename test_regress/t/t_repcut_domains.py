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
flags = [
    '--assert', '--stats', '--no-skip-identical', '--x-initial', '0', '--x-assign', '0',
    '--output-split-cfuncs', '32', '--output-split', '256'
]
base = Path(test.obj_dir) / (test.vm_prefix + '__repcut_early')
reference = {}
modes = (('stock', 1), ('serial', 1), ('parallel', 2), ('parallel4', 4), ('protected', 2),
         ('public', 2), ('no_gate', 2), ('shared_serial', 1), ('shared2', 2), ('shared4', 4),
         ('shared_protected', 2), ('shared_public', 2), ('shared_tiny', 2), ('shared_no_gate', 2))
modes += (('stock_large', 1), ('shared_large', 2), ('shared_large_tiny', 2),
          ('shared_large_protected', 2), ('default_large', 2), ('unshared_large', 2))
for mode, threads in modes:
    rounds = 8 if 'large' in mode else 0
    options = flags + (['--protect-ids', '--protect-key', 'REPCUT_DOMAINS'] if mode
                       in ('protected', 'shared_protected', 'shared_large_protected') else [])
    options += ['-GSHARED_ROUNDS=' + str(rounds)]
    shared = mode.startswith(('shared', 'default'))
    limit = 128 if (shared or mode == 'unshared_large') and 'tiny' not in mode else 32
    options += ['--output-split-cfuncs', str(limit)]
    if mode.startswith('shared'):
        options += ['--repcut-share-domains']
    elif mode == 'unshared_large':
        options += ['--no-repcut-share-domains']
    if mode in ('public', 'shared_public'):
        options += ['--public-flat-rw']
    if mode in ('no_gate', 'shared_no_gate') or rounds:
        options += ['-fno-gate']
    if mode == 'shared_large':
        options += ['--dumpi-tree', '3', '--dumpi-tree-json', '3']
    runtime = []
    if not mode.startswith('stock'):
        test.compile(make_main=False,
                     verilator_make_gmake=False,
                     threads=threads,
                     verilator_flags2=options + ['--repcut-dump-graph'])
        with base.with_suffix('.nodes.tsv').open(encoding="utf8") as stream:
            nodes = list(csv.DictReader(stream, delimiter='\t'))
        dependencies = [[int(x) for x in n['inputs'].split(',') if x] for n in nodes]
        with base.with_suffix('.controls.tsv').open(encoding="utf8") as stream:
            for row in csv.DictReader(stream, delimiter='\t'):
                dependencies[int(row['node'])].append(int(row['predicate']))
        parts = [set() for _ in range(threads)]
        sinks = [
            int(n['id']) for n in nodes if n['kind'] in ('NEXT', 'OUTPUT', 'MEMWRITE', 'EFFECT')
        ]
        for ordinal, sink in enumerate(sinks):
            pending = [sink]
            part = parts[ordinal % threads]
            while pending:
                node = pending.pop()
                if node not in part:
                    part.add(node)
                    pending.extend(dependencies[node])
        membership = Path(test.obj_dir) / 'membership.csv'
        membership.write_text(''.join(','.join(map(str, sorted(p))) + ',\n' for p in parts),
                              encoding="utf8")
        runtime = ['--repcut', '--repcut-partitioner', partitioner(test, membership)]
    test.compile(make_main=False,
                 verilator_make_gmake=False,
                 threads=threads,
                 verilator_flags2=options + runtime +
                 ['--exe', str(Path(test.pli_filename).resolve())])
    subprocess.run(['make', '-C', test.obj_dir, '-f', test.vm_prefix + '.mk', '-j2'], check=True)
    result = subprocess.run([str(Path(test.obj_dir).resolve() / test.vm_prefix)],
                            capture_output=True,
                            text=True,
                            check=True,
                            timeout=30)
    output = result.stdout + result.stderr
    (Path(test.obj_dir) / (mode + '.log')).write_text(output, encoding="utf8")
    assert '*-* All Finished *-*' in output
    if mode.startswith('stock'):
        reference[rounds] = output
    else:
        assert output == reference[rounds], mode
        # Public aliases remain observable. Disabling gate optimization still
        # allows later native sensitivity coalescing; distinct events stay separate.
        preserve_aliases = mode in ('public', 'shared_public')
        stats = Path(test.obj_dir) / (mode + '.stats')
        stats.write_text(Path(test.stats).read_text(encoding="utf8"), encoding="utf8")
        if mode == 'unshared_large':
            assert 'RepCut shared domains,' not in stats.read_text(encoding="utf8")
        test.file_grep(str(stats), r'RepCut native, domains\s+(\d+)',
                       16 if preserve_aliases else 6)
        test.file_grep(str(stats), r'RepCut native, coalesced sensitivities\s+(\d+)',
                       0 if preserve_aliases else 10)
        if threads > 1 and not preserve_aliases:
            test.file_grep(str(stats), r'RepCut native batch, conditional dispatches\s+(\d+)', 1)
            if shared:
                if rounds:
                    test.file_grep(str(stats), r'RepCut shared domains, factored helpers\s+(\d+)',
                                   threads + 1)
                    test.file_grep(str(stats),
                                   r'RepCut shared domains, partitions above cost floor\s+(\d+)',
                                   threads)
                else:
                    test.file_grep(
                        str(stats),
                        r'RepCut shared domains, cheap drivers retained locally\s+(\d+)', threads)
                    test.file_grep(str(stats),
                                   r'RepCut shared domains, partitions below cost floor\s+(\d+)',
                                   threads)
                    assert 'RepCut shared domains, factored helpers' not in stats.read_text(
                        encoding="utf8")
                if mode == 'shared_large':
                    dump = Path(test.glob_one(test.obj_dir + '/' + test.vm_prefix + '.tree.json'))
                    assert re.search(r'"repcutSharedOrder":\s*[1-9]',
                                     dump.read_text(encoding="utf8"))
                    dump = Path(
                        test.glob_one(test.obj_dir + '/' + test.vm_prefix + '_990_final.tree'))
                    assert 'repcutSharedOrder=' in dump.read_text(encoding="utf8")
        with base.with_suffix('.chunks.tsv').open(encoding="utf8") as stream:
            chunks = list(csv.DictReader(stream, delimiter='\t'))
        # A small partition may fit entirely in its entry function. Both entry
        # functions and any required helpers must obey the construction budget.
        assert chunks
        assert all(int(row['body_nodes']) <= limit for row in chunks)
test.passes()
