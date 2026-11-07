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
membership = Path(test.obj_dir) / 'membership.csv'
auto = '--repcut-parallel-publish-auto'
on = '--repcut-parallel-publish'
off = '--no-repcut-parallel-publish'
cases = [('stock', 2, [], 0), ('default', 2, [], 1),
         ('default4', 4, [], 1), ('auto', 2, [auto], 1), ('auto4', 4, [auto], 1),
         ('default-serial', 1, [], 0), ('serial', 1, [auto], 0), ('explicit-off', 2, [off], 0),
         ('skew', 2, [auto], 1), ('protect', 2, [auto, '--protect-ids', '-Wno-INSECURE'], 1),
         ('force-on', 2, [auto, on], 3), ('force-off', 2, [auto, off], 0),
         ('last-auto', 2, [on, off, auto], 1), ('guard-stock', 2, [], 0), ('guard', 2, [on], 3),
         ('guard-default', 4, [], 1), ('guard-auto', 4, [auto], 1),
         ('guard-protect', 2, [on, '--protect-ids', '-Wno-INSECURE'], 3)]
# Exercise the bounded conditional path before the ordinary-policy cases.
cases = [c for c in cases if c[0].startswith('guard')
         ] + [c for c in cases if not c[0].startswith('guard')]
reference = str(Path(test.obj_dir).resolve() / 'stock.vcd')
for case, threads, options, expected in cases:
    conditional = case.startswith('guard')
    flags = [
        '--trace-vcd', '--stats', '--debug-check', '--no-skip-identical', '--x-initial', '0',
        '--x-assign', '0', '--output-split-cfuncs', '256'
    ]
    if conditional:
        flags += ['-GCONDITIONAL=1', '-fno-table', '--output-split-cfuncs', '32']
    runtime = []
    if not case.endswith('stock'):
        test.compile(make_main=False,
                     verilator_make_gmake=False,
                     threads=threads,
                     verilator_flags2=flags + ['--repcut-dump-graph'])
        with base.with_suffix('.nodes.tsv').open(encoding="utf8") as stream:
            nodes = list(csv.DictReader(stream, delimiter='\t'))
        deps = [[int(x) for x in n['inputs'].split(',') if x] for n in nodes]
        with base.with_suffix('.controls.tsv').open(encoding="utf8") as stream:
            for row in csv.DictReader(stream, delimiter='\t'):
                deps[int(row['node'])].append(int(row['predicate']))
        parts = [set() for _ in range(threads)]
        ordinal = 0
        for n in nodes:
            if n['kind'] not in ('NEXT', 'OUTPUT', 'MEMWRITE', 'EFFECT'):
                continue
            owner = ordinal % threads
            ordinal += 1
            if n['kind'] == 'NEXT' and 'wide_a' in n['label']:
                index = int(re.search(r'__BRA__(\d+)__', n['label'])[1])
                owner = (index // 2 if conditional else index) % threads
            if n['kind'] == 'NEXT' and 'auxiliary' in n['label']:
                owner = 0 if conditional else threads - 1
            if n['kind'] == 'MEMWRITE' and conditional:
                owner = 1
            if n['kind'] == 'NEXT' and 'narrow' in n['label']:
                owner = 0
            if n['kind'] == 'NEXT' and 'wide_c' in n['label']:
                owner = 1 if case == 'skew' and '__BRA__127__' in n['label'] else 0
            pending = [int(n['id'])]
            while pending:
                node = pending.pop()
                if node not in parts[owner]:
                    parts[owner].add(node)
                    pending.extend(deps[node])
        assert set().union(*parts) == set(range(len(nodes)))
        membership.write_text(''.join(','.join(map(str, sorted(p))) + '\n' for p in parts),
                              encoding="utf8")
        runtime = ['--repcut', '--repcut-partitioner', partitioner(test, membership)] + options
    test.compile(make_main=False,
                 verilator_make_gmake=False,
                 threads=threads,
                 verilator_flags2=flags + runtime +
                 ['--exe', str(Path(test.pli_filename).resolve())])
    if not case.endswith('stock'):
        with base.with_suffix('.publication.tsv').open(encoding="utf8") as stream:
            rows = list(csv.DictReader(stream, delimiter='\t'))
        domains = {r['domain']: r for r in rows}
        assert len(domains) == 3
        assert sum(r['decision'] == 'parallel' for r in domains.values()) == expected, rows
        if case in ('default', 'default4', 'auto', 'auto4', 'protect', 'last-auto', 'skew'):
            assert {r['mode'] for r in rows} == {'auto'}
            reasons = {r['reason'] for r in rows}
            assert 'estimated-benefit' in reasons and 'dispatch-cost' in reasons
            assert ('owner-imbalance' if case == 'skew' else 'one-owner') in reasons
            if case == 'skew':
                assert 'owner-imbalance' in reasons
        if case in ('default-serial', 'serial'):
            assert {r['reason'] for r in rows} == {'single-thread'}
        if case in ('explicit-off', 'force-off'):
            assert {r['mode'] for r in rows} == {'off'}
            assert {r['reason'] for r in rows} == {'forced-off'}
        stats = Path(test.stats).read_text(encoding="utf8")
        guarded = re.search(r'RepCut native publication, guarded dispatches\s+(\d+)', stats)
        expected_guards = expected if conditional else int(case == 'force-on')
        assert (int(guarded[1]) if guarded else 0) == expected_guards, case
        if conditional:
            helpers = re.search(r'RepCut native publication, dispatch enable helpers\s+(\d+)',
                                stats)
            assert helpers and int(
                helpers[1]) == (13 if case in ('guard-default', 'guard-auto') else 15), case
        (Path(test.obj_dir) / (case + '.stats')).write_text(stats, encoding="utf8")
        (Path(test.obj_dir) / (case + '.publication.tsv')).write_text(
            base.with_suffix('.publication.tsv').read_text(encoding="utf8"), encoding="utf8")
    subprocess.run(['make', '-C', test.obj_dir, '-f', test.vm_prefix + '.mk', '-j2'], check=True)
    trace = str(Path(test.obj_dir).resolve() / (case + '.vcd'))
    result = subprocess.run([
        str(Path(test.obj_dir).resolve() / test.vm_prefix), trace,
        str(threads),
        str(int(conditional))
    ],
                            capture_output=True,
                            text=True,
                            timeout=60,
                            check=False)
    output = result.stdout + result.stderr
    (Path(test.obj_dir) / (case + '.log')).write_text(output, encoding="utf8")
    result.check_returncode()
    assert '*-* All Finished *-*' in output
    if not case.endswith(('stock', 'protect')):
        test.trace_identical(
            trace,
            str(Path(test.obj_dir).resolve() / 'guard-stock.vcd') if conditional else reference)
test.passes()
