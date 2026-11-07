#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2024 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios('vltmt')
test.top_filename = "t/t_dpi_threads.v"

test.skip_if_too_few_cores()

test.compile(v_flags2=["t/t_dpi_threads_c.cpp --threads-dpi all --no-threads-coarsen"])

# Similar to t_dpi_threads, which confirms that Verilator can prevent a
# race between DPI import calls, this test confirms that the race exists
# and that the DPI C code can detect it under --threads-dpi all
# mode.
#
# RepCut retains DPI effects on the caller, even with --threads-dpi all.
test.execute(fails=not test.repcut)
if test.repcut:
    test.file_grep(test.run_log_filename, r'dpii_sys_task\(\) no collision')
    test.file_grep_not(test.run_log_filename, r'saw threads collide')

test.passes()
