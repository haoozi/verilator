// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Source-backed native RepCut partition programs
//
// Code available from: https://verilator.org
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2026-2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//*************************************************************************

#ifndef VERILATOR_V3REPCUTNATIVE_H_
#define VERILATOR_V3REPCUTNATIVE_H_

#include "V3RepCutEmit.h"

#include <array>
#include <functional>

class AstScope;

namespace V3RepCut {
using NativeValues = std::map<size_t, AstVarScope*>;
// Graph islands consume previously materialized source values and export named
// combinational values. These are immutable within one evaluation, not caches.
struct NativeGraphBoundary final {
    const NativeValues& m_inputs;
    const NativeValues& m_outputs;
};
using NativeGraphLower = std::function<std::vector<AstNodeStmt*>(const std::vector<size_t>&,
                                                                 const NativeGraphBoundary&)>;
enum class NativeLowering : uint8_t { GRAPH, SOURCE, HYBRID };

// Source drivers shared within one partition. Groups are ordered so every
// dependency precedes its consumers. Each group keeps its exact domain union;
// its values are valid only during the current joined compute invocation.
struct NativeSharedGroup final {
    std::vector<size_t> m_domains;
    std::vector<AstNodeStmt*> m_statements;
};
struct NativeSharedPartition final {
    NativeValues m_values;
    std::vector<NativeSharedGroup> m_groups;
};
std::vector<NativeSharedPartition> nativeSharedPrograms(AstScope& scope, const Graph& graph,
                                                        const Plan& plan, const Bindings& bindings,
                                                        size_t statementLimit);

// Retain eligible source procedures and lower exceptional dependency cones as
// local graph islands. GRAPH leaves the AST untouched for the caller's full
// graph path when no source materialization remains useful.
// Scalar candidates use slot zero for the value and optional slot one for a
// shared write enable. Memory candidates capture enable, address, value, and
// an optional partial-write bit offset.
NativeLowering nativeProgram(AstScope& scope, const Graph& graph, const Bindings& bindings,
                             const std::vector<size_t>& sequence, const std::string& tag,
                             uint32_t group, size_t statementLimit,
                             const std::map<size_t, AstVarScope*>& captures,
                             std::vector<Candidates>& candidates,
                             std::vector<std::vector<AstVarScope*>>& effectArguments,
                             std::vector<AstNodeStmt*>& statements, AstVarScope* effectPendingp,
                             const NativeGraphLower& lowerGraph,
                             const NativeValues& sharedInputs = {});
}  // namespace V3RepCut

#endif  // Guard
