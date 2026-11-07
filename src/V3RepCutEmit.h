// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Emit RepCut kernels and state publication
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

#ifndef VERILATOR_V3REPCUTEMIT_H_
#define VERILATOR_V3REPCUTEMIT_H_

#include "V3RepCutPlan.h"

#include <array>
#include <map>

class AstNetlist;
class AstVarScope;
class AstNodeExpr;
class AstNodeStmt;
class AstNodeAssign;
class AstSenTree;
class AstAlways;
namespace V3RepCut {
// Scalars use value and optional enable. Memories use enable, address, value,
// and an optional dynamic bit offset for a partial element write. Constant
// offsets stay in the graph and need no runtime candidate storage.
using Candidates = std::array<AstVarScope*, 4>;
// Capture independent read-only inputs in partitions while retaining the
// original scheduled foreign body and its DPI ABI. Reentrant callbacks remain
// entirely under ordinary scheduling.
struct Callback final {
    const AstNodeStmt* m_statementsp;
    std::vector<const AstVarScope*> m_inputps;
    std::vector<std::pair<const AstVarScope*, bool>> m_outputs;  // Variable, NBA
};
// Bindings are valid only during synchronous normalization and lowering, before
// any later pass can delete or rewrite storage. Never retained in Graph or Plan.
struct Bindings final {
    std::vector<AstVarScope*> m_storageps;
    std::vector<const AstNodeExpr*> m_contextps;
    std::vector<const AstNodeStmt*> m_effectps;
    std::vector<Callback> m_callbacks;
    // Native callbacks retain their original process and event scheduling. Only
    // read-only scalar arguments and completely driven array elements are
    // computed and captured by the partitions.
    std::map<size_t, AstAlways*> m_nativeCallbackps;
    std::vector<const AstSenTree*> m_domainps;
    std::vector<AstAlways*> m_clockedps;
    std::vector<const AstAlways*> m_retainedps;
    std::map<size_t, const AstNodeAssign*> m_combAssignments;
    std::map<const Vertex*, size_t> m_sinkProcedures;  // Index in m_clockedps
    std::map<const AstNodeAssign*, const Vertex*> m_memoryWrites;
    std::map<const AstNodeStmt*, const Vertex*> m_effects;
};
void emit(AstNetlist* netlistp, const Graph& graph, const Plan& plan, const Bindings& bindings);
}  // namespace V3RepCut
#endif  // Guard
