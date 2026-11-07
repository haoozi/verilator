#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026-2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import csv
import json
import re
import subprocess
from pathlib import Path

import vltest_bootstrap
from repcut_common import partitioner


def check_settling(path):
    """Packed outputs must settle only on the clock that writes their bank."""
    tree = json.loads(path.read_text(encoding="utf8"))

    def nodes(value):
        if isinstance(value, dict):
            yield value
            for child in value.values():
                yield from nodes(child)
        elif isinstance(value, list):
            for child in value:
                yield from nodes(child)

    funcs = {n['addr']: n for n in nodes(tree) if n['type'] == 'CFUNC'}
    targets = {'banks_a': 1, 'banks_b': 2}
    relevant = set()
    for addr, func in funcs.items():
        if any(n['type'] == 'VARREF' and n['name'] in targets and n['access'] == 'WR'
               for n in nodes(func['stmtsp'])):
            relevant.add(addr)
    while True:
        callers = {
            addr
            for addr, func in funcs.items()
            if any(n['type'] == 'CCALL' and n['funcp'] in relevant for n in nodes(func['stmtsp']))
        }
        if callers <= relevant:
            break
        relevant |= callers
    found = set()

    def inspect(body, guards):
        for node in body:
            if node['type'] == 'IF':
                assert not node['elsesp']
                cond = node['condp'][0]
                assert cond['type'] == 'AND', cond
                assert cond['rhsp'][0]['fromp'][0]['name'] == '__VnbaTriggered'
                mask = int(cond['lhsp'][0]['name'].split("'h")[1], 16)
                inspect(node['thensp'], guards + [mask])
            elif node['type'] == 'CCALL':
                if node['funcp'] in relevant:
                    inspect(funcs[node['funcp']]['stmtsp'], guards)
            elif node['type'] == 'VARREF' and node['access'] == 'WR' and node['name'] in targets:
                assert guards == [targets[node['name']]], (node['name'], guards)
                found.add(node['name'])
            else:
                for child in node.values():
                    if isinstance(child, list):
                        inspect(child, guards)

    inspect(funcs[tree['evalNbap']]['stmtsp'], [])
    assert found == targets.keys(), found


def check_output_packing(path):  # pylint: disable=too-many-locals
    """Shared boundary words and mixed-clock outputs must stay on the caller."""
    tree = json.loads(path.read_text(encoding="utf8"))

    def nodes(value):
        if isinstance(value, dict):
            if 'type' in value:
                yield value
            for child in value.values():
                yield from nodes(child)
        elif isinstance(value, list):
            for child in value:
                yield from nodes(child)

    funcs = {n['addr']: n for n in nodes(tree) if n['type'] == 'CFUNC'}
    owners = {}
    for addr, func in funcs.items():
        if not func.get('repcutLocalPublish'):
            continue
        pending = [addr]
        seen = set()
        while pending:
            current = pending.pop()
            if current in seen:
                continue
            seen.add(current)
            body = funcs[current]['stmtsp']
            packed = False
            for node in nodes(body):
                if node['type'] == 'CCALL':
                    pending.append(node['funcp'])
                if node['type'] != 'ASSIGN':
                    continue
                lhs = node['lhsp'][0]
                if lhs['type'] != 'WORDSEL' or lhs['fromp'][0]['type'] != 'VARREF':
                    continue
                name = lhs['fromp'][0]['name']
                assert name not in ('mixed', 'memory_out'), name
                if name not in ('banks_a', 'banks_b'):
                    continue
                packed = True
                word = int(lhs['bitp'][0]['name'].split("'h")[1], 16)
                key = (name, word)
                assert owners.setdefault(key, addr) == addr, key
            if packed:
                assert funcs[current].get('noInline')
                assert len(list(nodes(body))) <= 32
    expected = {(name, word)
                for name in ('banks_a', 'banks_b')
                for word in (0, 1, 3, 4, 6, 7, 9, 10, 11)}
    assert owners.keys() == expected, owners.keys()


test.scenarios('vltmt')
test.top_filename = str(Path(test.top_filename).resolve())
flags = [
    '--assert', '--stats', '--no-skip-identical', '--x-initial', '0', '--x-assign', '0',
    '--output-split-cfuncs', '32', '--output-split', '256', '-fno-table'
]
base = Path(test.obj_dir) / (test.vm_prefix + '__repcut_early')
references = {}
for mode, threads in [('stock', 1), ('serial', 1), ('parallel', 2), ('parallel4', 4),
                      ('packing', 2), ('protected', 2), ('public', 2), ('trace', 2), ('caller', 2),
                      ('isolated_stock', 1), ('isolated', 2)]:
    options = flags[:]
    if mode == 'packing':
        options[options.index('--output-split-cfuncs') + 1] = '4000'
    isolated = mode.startswith('isolated')
    if isolated:
        options += ['+define+ISOLATED', '-CFLAGS', '-DISOLATED']
    if mode == 'parallel':
        options += ['--dumpi-tree', '3', '--dumpi-tree-json', '3']
    if mode == 'protected':
        options += ['--protect-ids', '--protect-key', 'REPCUT_CONCURRENT']
    if mode == 'public':
        options += ['--public-flat-rw']
    if mode == 'trace':
        options += ['--trace']
    if mode == 'caller':
        options += ['--no-repcut-parallel-publish']
    runtime = []
    if not mode.endswith('stock'):
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
        for node in nodes:
            if node['kind'] not in ('NEXT', 'OUTPUT', 'MEMWRITE', 'EFFECT'):
                continue
            label = node['label']
            owner = 0
            if any(name in label for name in ('right_state', 'guarded', 'memory')):
                owner = 1 % threads
            match = re.search(r'banks__BRA__(\d+)__KET__', label)
            if match:
                owner = int(match[1]) % threads
            pending = [int(node['id'])]
            while pending:
                ident = pending.pop()
                if ident not in parts[owner]:
                    parts[owner].add(ident)
                    pending.extend(dependencies[ident])
        membership = Path(test.obj_dir) / 'membership.csv'
        membership.write_text(''.join(','.join(map(str, sorted(p))) + ',\n' for p in parts),
                              encoding="utf8")
        runtime = ['--repcut', '--repcut-partitioner', partitioner(test, membership)]
    test.compile(make_main=False,
                 verilator_make_gmake=False,
                 threads=threads,
                 verilator_flags2=options + runtime +
                 ['--exe', str(Path(test.pli_filename).resolve())])
    if mode == 'parallel':
        trees = list(Path(test.obj_dir).glob('*.tree'))
        json_trees = list(Path(test.obj_dir).glob('*.tree.json'))
        assert any('[REPCUTBATCH]' in p.read_text(encoding="utf8") for p in trees)
        assert any(
            re.search(r'"repcutNativeBatch":\s*true', p.read_text(encoding="utf8"))
            for p in json_trees)
        assert any('[REPCUTLOCALPUBLISH]' in p.read_text(encoding="utf8") for p in trees)
        assert any(
            re.search(r'"repcutLocalPublish":\s*true', p.read_text(encoding="utf8"))
            for p in json_trees)
        check_settling(max(Path(test.obj_dir).glob('*_sched.tree.json')))
        check_output_packing(
            Path(test.glob_one(str(Path(test.obj_dir) / '*_repcut_output.tree.json'))))
    subprocess.run(['make', '-C', test.obj_dir, '-f', test.vm_prefix + '.mk', '-j2'], check=True)
    result = subprocess.run([str(Path(test.obj_dir).resolve() / test.vm_prefix)],
                            capture_output=True,
                            text=True,
                            timeout=60,
                            check=False)
    output = result.stdout + result.stderr
    (Path(test.obj_dir) / (mode + '.log')).write_text(output, encoding="utf8")
    assert result.returncode == 0, (mode, output)
    assert '*-* All Finished *-*' in output
    if mode.endswith('stock'):
        references[isolated] = output
    else:
        assert output == references[isolated], mode
        stats = Path(test.stats).read_text(encoding="utf8")
        stats_path = Path(test.obj_dir) / (mode + '.stats')
        stats_path.write_text(stats, encoding="utf8")
        eligible = threads > 1 and mode not in ('public', 'trace')
        if threads > 1:
            words = 24 if mode == 'protected' else 18 if eligible and mode != 'caller' else 0
            test.file_grep(str(stats_path), r'RepCut native output, moved words\s+(\d+)', words)
        test.file_grep(str(stats_path), r'RepCut native concurrent, eligible\s+(\d+)',
                       int(eligible))
        if eligible:
            test.file_grep(str(stats_path), r'RepCut native batch, domains\s+(\d+)', 3)
            test.file_grep(str(stats_path), r'RepCut native batch, dispatches\s+(\d+)', 1)
            # Cross-clock readers on the same owner, cross-partition readers and
            # old-state predicates all require ordinary staged publication.
            test.file_grep(str(stats_path),
                           r'RepCut native concurrent, blocked shared state\s+(\d+)',
                           0 if isolated or mode == 'caller' else 5)
            test.file_grep(str(stats_path),
                           r'RepCut native concurrent, locally published scalars\s+(\d+)',
                           0 if mode == 'caller' else 9 if isolated else 10)
        with base.with_suffix('.chunks.tsv').open(encoding="utf8") as stream:
            chunks = list(csv.DictReader(stream, delimiter='\t'))
        limit = 4000 if mode == 'packing' else 32
        assert chunks and all(int(row['body_nodes']) <= limit for row in chunks)
test.passes()
