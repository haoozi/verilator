#!/usr/bin/env python3
# DESCRIPTION: Verilator: Common RepCut regression support
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026-2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import shlex
from pathlib import Path


def partitioner(test, membership):
    """Provide controlled test ownership through the external tool protocol."""
    executable = (Path(test.obj_dir) / 'test_partitioner.py').resolve()
    executable.write_text('''#!/usr/bin/env python3
import sys
from pathlib import Path
args = dict(zip(sys.argv[1::2], sys.argv[2::2]))
prefix = Path(args['--graph_file']).name.removesuffix('__repcut_early.graph')
source = Path(''' + repr(str(Path(membership).resolve())) + '''.replace('%p', prefix))
output = Path(args['--work_directory']) / 'rcp_output.txt'
output.write_bytes(source.read_bytes())
''',
                          encoding="utf8")
    executable.chmod(0o755)
    return shlex.quote(str(executable))
