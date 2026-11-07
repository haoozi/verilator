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

test.scenarios('vlt_all')

for used, protect in ((False, False), (True, False), (True, True)):
    flags = ['--no-skip-identical']
    if used:
        flags += ['+define+USE_CLASS']
    if protect:
        flags += ['--protect-ids', '--protect-key', 'CLASS_GC_TEST']
    test.compile(threads=2 if test.scenario == 'vltmt' else 1, verilator_flags2=flags)
    model = (Path(test.obj_dir) / (test.vm_prefix + '.cpp')).read_text(encoding="utf8")
    syms = (Path(test.obj_dir) / (test.vm_prefix + '__Syms.h')).read_text(encoding="utf8")
    assert ('__Vm_deleter.deleteAll()' in model) == used, 'Incorrect garbage collection call'
    assert ('VlDeleter __Vm_deleter;' in syms) == used, 'Incorrect garbage collector storage'
    test.execute()

test.passes()
