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
for fmt in ('vcd', 'fst'):
    reference = str(Path(test.obj_dir).resolve() / ('stock.' + fmt))
    for case, threads, split in (('stock', 2, 64), ('off', 2, 64), ('on', 2, 64), ('on4', 4, 64),
                                 ('unsplit', 2, 0)):
        flags = [
            '--trace-' + fmt, '--trace-structs', '--stats', '--no-skip-identical', '--debug-check',
            '--x-initial', '0', '--x-assign', '0', '--output-split-cfuncs',
            str(split)
        ]
        runtime = []
        if case != 'stock':
            test.compile(make_main=False,
                         verilator_make_gmake=False,
                         threads=threads,
                         verilator_flags2=flags + ['--repcut-dump-graph'])
            assert 'complete 1\n' in base.with_suffix('.report.txt').read_text(encoding="utf8")
            with base.with_suffix('.nodes.tsv').open(encoding="utf8") as stream:
                nodes = list(csv.DictReader(stream, delimiter='\t'))
            deps = [[int(x) for x in n['inputs'].split(',') if x] for n in nodes]
            with base.with_suffix('.controls.tsv').open(encoding="utf8") as stream:
                for row in csv.DictReader(stream, delimiter='\t'):
                    deps[int(row['node'])].append(int(row['predicate']))
            parts = [set() for _ in range(threads)]
            sinks = [
                int(n['id']) for n in nodes
                if n['kind'] in ('NEXT', 'OUTPUT', 'MEMWRITE', 'EFFECT')
            ]
            for ordinal, sink in enumerate(sinks):
                pending = [sink]
                part = parts[ordinal % threads]
                while pending:
                    node = pending.pop()
                    if node not in part:
                        part.add(node)
                        pending.extend(deps[node])
            assert set().union(*parts) == set(range(len(nodes)))
            membership.write_text(''.join(','.join(map(str, sorted(p))) + '\n' for p in parts),
                                  encoding="utf8")
            runtime = ['--repcut', '--repcut-partitioner', partitioner(test, membership)]
            if case != 'off':
                runtime += ['--repcut-parallel-publish']
        test.compile(make_main=False,
                     verilator_make_gmake=False,
                     threads=threads,
                     verilator_flags2=flags + runtime +
                     ['--exe', str(Path(test.pli_filename).resolve())])
        stats = Path(test.stats).read_text(encoding="utf8")
        # file_grep caches contents, so use a fresh filename for every build.
        stats_path = Path(test.obj_dir) / (case + '-' + fmt + '.stats')
        stats_path.write_text(stats, encoding="utf8")
        if case not in ('stock', 'off'):
            test.file_grep(str(stats_path), r'RepCut native publication, dispatches\s+(\d+)', 2)
            test.file_grep(str(stats_path), r'RepCut native publication, parallel scalars\s+(\d+)',
                           65)
        trace_check = Path(test.obj_dir) / (case + '-' + fmt + '.trace.txt')
        trace_check.write_text(
            (Path(test.obj_dir) / (test.vm_prefix + '__Trace__0.cpp')).read_text(encoding="utf8"),
            encoding="utf8")
        test.file_grep(str(trace_check), r'trace_chg_dtype.*structured_state')
        # Keep a real conditional trace path: always-tracing would mask a missing
        # caller activity edge even if the register oracle still passes.
        assert re.search(r'Tracing, Activity setters\s+[1-9]\d*', stats), case
        subprocess.run(['make', '-C', test.obj_dir, '-f', test.vm_prefix + '.mk', '-j2'],
                       check=True)
        trace = str(Path(test.obj_dir).resolve() / (case + '.' + fmt))
        result = subprocess.run(
            [str(Path(test.obj_dir).resolve() / test.vm_prefix), trace,
             str(threads)],
            capture_output=True,
            text=True,
            timeout=30,
            check=False)
        output = result.stdout + result.stderr
        (Path(test.obj_dir) / (case + '-' + fmt + '.log')).write_text(output, encoding="utf8")
        result.check_returncode()
        assert '*-* All Finished *-*' in output
        if case != 'stock':
            test.trace_identical(trace, reference)
test.passes()
