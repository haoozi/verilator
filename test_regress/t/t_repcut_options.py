#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026-2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import json
import os
import shlex
import subprocess
from pathlib import Path

import vltest_bootstrap

test.scenarios('vltmt')
test.top_filename = 't/t_repcut_native.v'
directory = Path(test.obj_dir)
base = directory / (test.vm_prefix + '__repcut_early')
tool_dir = directory / "tools with space's"
tool_dir.mkdir(exist_ok=True)
partitioner = tool_dir / 'rcp'
partitioner.write_text('''#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
p = argparse.ArgumentParser()
for key in ('graph_file', 'work_directory', 'nparts', 'threads', 'seed', 'target_ib'):
    p.add_argument('--' + key, required=True)
a = p.parse_args()
work = Path(a.work_directory)
with (work / 'invocations.jsonl').open('a') as f:
    f.write(json.dumps(vars(a)) + '\\n')
lines = Path(a.graph_file).read_text().splitlines()
count = int(lines[0].split()[1])
deps = [[] for _ in range(count)]
sinks = []
for node, line in enumerate(lines[1:]):
    outputs = [int(x) for x in line.split()[2:]]
    if not outputs:
        sinks.append(node)
    for output in outputs:
        deps[output].append(node)
parts = [set() for _ in range(int(a.nparts))]
for i, sink in enumerate(sinks):
    part = parts[i % len(parts)]
    pending = [sink]
    while pending:
        node = pending.pop()
        if node not in part:
            part.add(node)
            pending.extend(deps[node])
(work / 'rcp_output.txt').write_text(''.join(
    ','.join(map(str, sorted(part))) + '\\n' for part in parts))
''',
                       encoding="utf8")
partitioner.chmod(0o755)
driver = directory / 'driver.cpp'
driver.write_text(Path('t/t_repcut_native.cpp').read_text(encoding="utf8").replace(
    'Vt_repcut_native', test.vm_prefix),
                  encoding="utf8")
flags = [
    '--no-skip-identical', '--x-initial', '0', '--x-assign', '0', '--exe',
    str(driver.resolve())
]
stock_code = {}
reference = None
saved_path = os.environ['PATH']
calls = Path(str(base) + '.partition') / 'invocations.jsonl'
calls.unlink(missing_ok=True)
os.environ['PATH'] = str(tool_dir.resolve()) + os.pathsep + saved_path
try:
    for threads in (1, 2):
        for case in ('stock', 'disabled', 'dormant', 'automatic', 'override'):
            # Remove only generated model sources so stale split files cannot
            # affect the equality check or the next model build.
            for pattern in (test.vm_prefix + '*.cpp', test.vm_prefix + '*.h'):
                for path in directory.glob(pattern):
                    path.unlink()
            extra = []
            if case == 'disabled':
                extra = ['--no-repcut']
            elif case == 'dormant':
                extra = [
                    '--repcut-partitioner', 'missing-partitioner', '--repcut-parallel-publish'
                ]
            elif case == 'automatic':
                extra = ['--repcut']
            elif case == 'override':
                extra = ['--repcut', '--no-repcut', '--repcut-partitioner', 'missing-partitioner']
            test.compile(make_main=False, threads=threads, verilator_flags2=flags + extra)
            assert test.repcut == (case == 'automatic'), (threads, case)
            code = {
                p.name: p.read_bytes()
                for pattern in (test.vm_prefix + '*.cpp', test.vm_prefix + '*.h')
                for p in directory.glob(pattern)
            }
            if case == 'stock':
                stock_code[threads] = code
            if case in ('stock', 'disabled', 'dormant', 'override'):
                assert code == stock_code[threads], (threads, case)
                assert not any(b'__Vrepcut' in data for data in code.values())
            if case == 'automatic':
                generated = Path(str(base) + '.partition') / 'rcp_output.txt'
                assert len(generated.read_text(encoding="utf8").splitlines()) == threads
            result = subprocess.run([str((directory / test.vm_prefix).resolve())],
                                    capture_output=True,
                                    text=True,
                                    timeout=30,
                                    check=True)
            if reference is None:
                reference = result.stdout
                assert '*-* All Finished *-*' in reference
            assert result.stdout == reference, (threads, case)
    invocations = [json.loads(line) for line in calls.read_text(encoding="utf8").splitlines()]
    assert len(invocations) == 1, 'Disabled or serial mode invoked the partitioner'
    assert invocations[0]['nparts'] == '2'
    assert invocations[0]['threads'] == '1'
    assert invocations[0]['seed'] == '42'
    assert invocations[0]['target_ib'] == '0.03'
    # Also exercise an explicit executable path containing spaces and a quote.
    test.compile(make_main=False,
                 verilator_make_gmake=False,
                 threads=2,
                 verilator_flags2=flags +
                 ['--repcut', '--repcut-partitioner',
                  shlex.quote(str(partitioner.resolve()))])
    # Tiny and empty graphs must preserve idle worker slots without invoking an
    # external hypergraph partitioner on an edgeless partitioning problem.
    trivial = directory / 'trivial.v'
    for rtl in ('module t; endmodule\n', ('module t(input logic clk, d, output logic q); '
                                          'always @(posedge clk) q <= d; endmodule\n')):
        trivial.write_text(rtl, encoding="utf8")
        test.top_filename = str(trivial.resolve())
        test.compile(make_main=False,
                     verilator_make_gmake=False,
                     threads=4,
                     verilator_flags2=[
                         '--repcut', '--repcut-partitioner', 'missing-partitioner',
                         '--no-skip-identical'
                     ])
        generated = Path(str(base) + '.partition') / 'rcp_output.txt'
        assert len(generated.read_text(encoding="utf8").splitlines()) == 4
    test.top_filename = 't/t_repcut_native.v'
    for mode in ('lint-only', 'dpi-hdr-only', 'json-only'):
        output = directory / mode
        test.compile(make_main=False,
                     verilator_make_gmake=False,
                     threads=2,
                     verilator_flags2=[
                         '--' + mode, '--Mdir',
                         str(output), '--repcut', '--repcut-partitioner', 'missing-partitioner',
                         '--no-skip-identical'
                     ])
        assert not list(output.glob('*__repcut*')), mode
finally:
    os.environ['PATH'] = saved_path

test.passes()
