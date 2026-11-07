#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import shutil
from pathlib import Path

import vltest_bootstrap

test.scenarios('vltmt')
if not (shutil.which('rcp') and shutil.which('MtKaHyPar')):
    test.skip('Optional RepCut partitioner and MtKaHyPar executables not found')

base = Path(test.obj_dir) / (test.vm_prefix + '__repcut_early')
for shared in (0, 1):
    test.compile(make_main=False,
                 threads=2,
                 verilator_flags2=[
                     '--repcut', '--no-skip-identical', '--x-initial', '0', '--x-assign', '0',
                     '-GSHARED=' + str(shared), '--exe',
                     str(Path(test.pli_filename).resolve()), '-CFLAGS',
                     '-DREPCUT_SHARED=' + str(shared)
                 ])
    # More cones than workers must reach the external partitioner. The first
    # case exercises rcp's independent-cone balancing; the second needs MtKaHyPar.
    hypergraph = base.with_suffix('.partition') / 'parts.hmetis'
    edges, cones, _ = map(int, hypergraph.read_text(encoding="utf8").splitlines()[0].split())
    assert cones > 2, cones
    assert (edges > 0) == bool(shared), (shared, edges)
    membership = base.with_suffix('.partition') / 'rcp_output.txt'
    parts = membership.read_text(encoding="utf8").splitlines()
    assert len(parts) == 2 and all(part.strip() for part in parts), parts
    test.execute()

test.passes()
