// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Bound RepCut compilation chunks with storage spans
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

#ifndef VERILATOR_V3REPCUTCHUNK_H_
#define VERILATOR_V3REPCUTCHUNK_H_

#include "config_build.h"
#include "verilatedos.h"

#include <vector>

class AstNodeStmt;
class AstScope;

namespace V3RepCut {
struct ChunkPlan final {
    // Boundary i is before statement i; the final boundary is statements.size().
    std::vector<size_t> m_prefixNodes;
    std::vector<uint64_t> m_liveValues;
    std::vector<uint64_t> m_liveBytes;
    std::vector<size_t> m_greedyEnds;
    std::vector<size_t> m_ends;
    bool m_analyzed = false;
};

// Keep statement order and the minimum number of bounded contiguous chunks.
// Among such splits, minimize summed conservative intermediate storage spans.
// A span covers first through last reference, including guarded references;
// it is an estimate of boundary traffic, not path-sensitive dynamic liveness.
// Generated callees are analyzed within a fixed traversal budget; opaque or
// over-budget access analysis falls back to the minimum-count greedy split.
ChunkPlan planChunks(const std::vector<AstNodeStmt*>& statements, const std::vector<size_t>& sizes,
                     size_t limit) VL_MT_DISABLED;
void selfTestChunks(AstScope& scope) VL_MT_DISABLED;
}  // namespace V3RepCut

#endif  // Guard
