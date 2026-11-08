#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

from pathlib import Path

import vltest_bootstrap
from repcut_common import partitioner

test.scenarios('vlt_all')
test.top_filename = 't/t_assert_ctl_arg.v'
threads = 2 if test.vltmt else 1
driver = Path(test.obj_dir) / 'driver.cpp'
driver.write_text(Path('t/t_assert_ctl_arg.cpp').read_text(encoding="utf8").replace(
    'contextp->threads(1);', f'contextp->threads({threads});'),
                  encoding="utf8")

flags = ['--assert', '--timing', '--coverage-user', '-fno-inline', '--no-skip-identical']
if threads > 1:
    # Exercise coverage in both workers without requiring external partitioners.
    # Graph edges include data and control dependencies for each sink's closure.
    test.compile(make_top_shell=False,
                 make_main=False,
                 verilator_make_gmake=False,
                 threads=threads,
                 verilator_flags2=flags + ['--repcut-dump-graph'])
    base = Path(test.obj_dir) / (test.vm_prefix + '__repcut_early')
    nodes = [
        line.split()
        for line in base.with_suffix('.graph').read_text(encoding="utf8").splitlines()[1:]
    ]
    predecessors = [[] for _ in nodes]
    for nid, node in enumerate(nodes):
        for successor in node[2:]:
            predecessors[int(successor)].append(nid)
    sinks = [nid for nid, node in enumerate(nodes) if float(node[1]) >= 0 and len(node) == 2]
    assert len(sinks) > threads
    parts = [set() for _ in range(threads)]
    for ordinal, sink in enumerate(sinks):
        part = parts[ordinal % threads]
        pending = [sink]
        while pending:
            nid = pending.pop()
            if nid not in part:
                part.add(nid)
                pending.extend(predecessors[nid])
    membership = Path(test.obj_dir) / 'membership.csv'
    membership.write_text(''.join(','.join(map(str, sorted(part))) + '\n' for part in parts),
                          encoding="utf8")
    flags += ['--repcut-partitioner', partitioner(test, membership)]

# The concurrent submodule has two permanently zero coverage bins. Its
# assertions move into RepCut kernels, but its coverage scope must remain.
test.compile(
    make_top_shell=False,
    make_main=False,
    threads=threads,
    verilator_flags2=flags +
    ['--repcut', '--exe',
     str(driver.resolve()), '-CFLAGS', '-I' + str(Path('t').resolve())])
test.execute(all_run_flags=['+verilator+error+limit+100'],
             expect_filename='t/t_assert_ctl_arg.out')
test.files_identical(test.coverage_filename, 't/t_assert_ctl_arg.dat.out')
test.passes()
