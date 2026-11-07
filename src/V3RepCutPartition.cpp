// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Invoke the optional RepCut partitioner
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2026-2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
//
// Automatic partitioning runs while the exported graph still has live AST
// bindings. Use one partition per simulation thread, with reproducible external
// settings. A single partition or empty graph needs no external tool. Each
// hierarchical model has its own prefix and working directory. Retain the
// partitioner's log and membership for diagnosis.
// Never accept stale output from a failed invocation, and validate generated
// membership before emitting code.
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3File.h"
#include "V3Os.h"
#include "V3RepCutPlan.h"
#include "V3String.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <memory>

namespace {

// V3Os::system uses the host shell. Quote every path as one literal argument;
// generated model directories and user-selected executables may contain spaces
// or shell metacharacters. Automatic partitioning requires a POSIX shell.
std::string shellArg(const std::string& text) {
    return "'" + VString::replaceSubstr(text, "'", "'\\''") + "'";
}

// With no more sinks than threads, each complete sink cone can have its own
// worker. This also handles empty models and avoids asking the hypergraph
// partitioner to split tiny, edgeless inputs. A generation tag avoids clearing
// a graph-sized membership vector for each worker.
void trivialPartitions(const V3RepCut::Graph& graph, const std::vector<size_t>& sinks,
                       size_t threads, std::ostream& output) {
    if (threads == 1) {
        for (const V3RepCut::Vertex* const vp : graph.m_vertexps) output << vp->m_id << ',';
        output << '\n';
        return;
    }
    std::vector<size_t> visited(graph.m_vertexps.size(), V3RepCut::NO_ID);
    for (size_t p = 0; p < threads; ++p) {
        std::vector<size_t> pending;
        std::vector<size_t> members;
        if (p < sinks.size()) pending.push_back(sinks[p]);
        while (!pending.empty()) {
            const size_t id = pending.back();
            pending.pop_back();
            if (visited[id] == p) continue;
            visited[id] = p;
            members.push_back(id);
            graph.m_vertexps[id]->foreachPredecessor(
                [&](const V3RepCut::Vertex& input, V3RepCut::DependencyKind) {
                    pending.push_back(input.m_id);
                    return false;
                });
        }
        std::sort(members.begin(), members.end());
        for (const size_t id : members) output << id << ',';
        output << '\n';
    }
}

}  // namespace

bool V3RepCut::Plan::partition(const Graph& graph, const std::string& base, Plan& result,
                               std::string& error) {
    if (graph.m_report.m_failures) {
        error = "Graph is incomplete or cyclic";
        return false;
    }
    const std::string directory = base + ".partition";
    const std::string filename = directory + "/rcp_output.txt";
    const std::string log = base + ".partition.log";
    const int threads = v3Global.opt.threads();
    V3Os::createDir(directory);
    if (std::remove(filename.c_str()) != 0 && errno != ENOENT) {
        error = "Cannot remove stale partitioner output '" + filename + "'";
        return false;
    }
    std::vector<size_t> sinks;
    for (const Vertex* const vp : graph.m_vertexps) {
        if (!vp->isSink()) continue;
        sinks.push_back(vp->m_id);
        if (sinks.size() > static_cast<size_t>(threads)) break;
    }
    if (threads == 1 || sinks.size() <= static_cast<size_t>(threads)) {
        const std::unique_ptr<std::ofstream> outputp{V3File::new_ofstream(filename)};
        trivialPartitions(graph, sinks, threads, *outputp);
        outputp->close();
        if (outputp->fail()) {
            error = "Cannot write automatic membership '" + filename + "'";
            return false;
        }
    } else {
        const std::string command = shellArg(v3Global.opt.repcutPartitioner()) + " --graph_file "
                                    + shellArg(base + ".graph") + " --work_directory "
                                    + shellArg(directory) + " --nparts " + cvtToStr(threads)
                                    + " --threads 1 --seed 42 --target_ib 0.03 > " + shellArg(log)
                                    + " 2>&1";
        const int status = V3Os::system(command);
        if (status) {
            error = "Partitioner exited with status " + cvtToStr(status) + "; see '" + log
                    + "'. Install rcp and MtKaHyPar on PATH, or select --repcut-partitioner";
            return false;
        }
        V3File::addTgtDepend(filename);
        V3File::addTgtDepend(log);
    }
    std::ifstream input{filename};
    if (!read(graph, input, result, error)) return false;
    if (result.m_partitions.size() != static_cast<size_t>(threads)) {
        error = "Partitioner returned " + cvtToStr(result.m_partitions.size()) + " partitions for "
                + cvtToStr(threads) + " threads";
        result = Plan{};
        return false;
    }
    return true;
}
