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
base = Path(test.obj_dir) / (test.vm_prefix + '__repcut_early')
for case, threads, extra in [('stock', 1, []), ('serial', 1, []), ('parallel', 2, []),
                             ('off', 2, ['--no-repcut-parallel-publish']),
                             ('publish', 2, ['--repcut-parallel-publish']),
                             ('four', 4, ['--repcut-parallel-publish']),
                             ('protected', 2, ['--repcut-parallel-publish', '--protect-ids']),
                             ('chunks', 2,
                              ['--repcut-parallel-publish', '--output-split-cfuncs', '32'])]:
    flags = [
        '--assert', '--stats', '--no-skip-identical', '--x-initial', '0', '--x-assign', '0',
        '--output-split-cfuncs', '4000', '-Wno-LITENDIAN', '-CFLAGS',
        f'-DREPCUT_TEST_THREADS={threads}'
    ] + extra
    runtime = []
    if case != 'stock':
        test.compile(make_main=False,
                     verilator_make_gmake=False,
                     threads=threads,
                     verilator_flags2=flags + ['--repcut-dump-graph'])
        report = base.with_suffix('.report.txt').read_text(encoding="utf8")
        assert 'complete 1\n' in report
        assert 'native_retained expression or value type: 1' in report
        with base.with_suffix('.nodes.tsv').open(encoding="utf8") as stream:
            nodes = list(csv.DictReader(stream, delimiter='\t'))
        deps = [[int(x) for x in n['inputs'].split(',') if x] for n in nodes]
        partial = [n for n in nodes if n['kind'] == 'MEMWRITE' and len(deps[int(n['id'])]) == 5]
        assert len(partial) == 8, (case, len(partial))
        with base.with_suffix('.controls.tsv').open(encoding="utf8") as stream:
            for row in csv.DictReader(stream, delimiter='\t'):
                deps[int(row['node'])].append(int(row['predicate']))
        parts = [set() for _ in range(threads)]
        memories = {}
        for ordinal, n in enumerate(nodes):
            if n['kind'] not in ('NEXT', 'OUTPUT', 'MEMWRITE', 'EFFECT'):
                continue
            owner = ordinal % threads
            if n['kind'] == 'MEMWRITE':
                # Different memories have different publication owners. At four
                # threads, each memory also has two candidate-producing workers.
                memory = memories.setdefault(deps[int(n['id'])][0], len(memories))
                owner = (2 * memory + ordinal % 2) % threads if threads == 4 else memory % threads
            pending = [int(n['id'])]
            while pending:
                node = pending.pop()
                if node not in parts[owner]:
                    parts[owner].add(node)
                    pending.extend(deps[node])
        assert set().union(*parts) == set(range(len(nodes)))
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
    if '--repcut-parallel-publish' in extra:
        count = re.search(r'RepCut native publication, partial memory ports\s+(\d+)', stats)
        assert count and int(count[1]) == 8, case
    (Path(test.obj_dir) / (case + '.stats')).write_text(stats, encoding="utf8")
    subprocess.run(['make', '-C', test.obj_dir, '-f', test.vm_prefix + '.mk', '-j2'], check=True)
    result = subprocess.run([str(Path(test.obj_dir).resolve() / test.vm_prefix)],
                            capture_output=True,
                            text=True,
                            timeout=60,
                            check=False)
    output = result.stdout + result.stderr
    (Path(test.obj_dir) / (case + '.log')).write_text(output, encoding="utf8")
    result.check_returncode()
    assert '*-* All Finished *-*' in output, case
test.passes()
