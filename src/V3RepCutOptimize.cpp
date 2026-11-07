// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Optimize guarded RepCut evaluation
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

// Propagate immutable values through typed copies without speculating guarded
// computation. A source must be a stable phase input or a single definition
// preceding the copy. A destination must have one full definition, no earlier
// reads, and no external publication obligation. Its guard must cover every use,
// and its source must be available at the copy. Reopened guards are matched only
// when their predicates are stable. Repeated writes and snapshots keep storage.
// A separate local-expression pass substitutes pure single-use packed values
// only within a straight-line block. Operand write versions prove availability;
// guard, call and memory boundaries retain storage. The expression-size budget
// keeps later compilation functions bounded.
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3RepCutOptimize.h"

#include "V3DfgPasses.h"
#include "V3Stats.h"

#include <array>
#include <limits>
#include <map>
#include <memory>

namespace {

struct Definition final {
    size_t m_writes = 0;
    size_t m_order = 0;
    size_t m_firstRead = std::numeric_limits<size_t>::max();
    const AstAssign* m_assignp = nullptr;
    size_t m_guard = 0;
    std::vector<size_t> m_readGuards;
};
using Aliases = std::map<AstVarScope*, AstVarScope*>;

class StateAccessVisitor final : public VNVisitorConst {
public:
    struct Access final {
        size_t m_reads = 0;
        size_t m_writes = 0;
        size_t m_lastRead = 0;
    };

private:
    std::map<AstVarScope*, Access> m_access;
    size_t m_statement = 0;
    void visit(AstVarRef* nodep) override {
        Access& varAccess = m_access[nodep->varScopep()];
        if (nodep->access().isReadOrRW()) {
            ++varAccess.m_reads;
            varAccess.m_lastRead = m_statement;
        }
        if (nodep->access().isWriteOrRW()) ++varAccess.m_writes;
    }
    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

public:
    explicit StateAccessVisitor(const std::vector<AstNodeStmt*>& statements) {
        for (AstNodeStmt* const stmtp : statements) {
            ++m_statement;
            // Include both branch bodies and their predicates. A later guarded
            // read is still a reason to retain the old-state snapshot.
            iterateConst(stmtp);
        }
    }
    Access access(AstVarScope* vscp) const {
        const auto it = m_access.find(vscp);
        return it == m_access.end() ? Access{} : it->second;
    }
};

// Order individual assignments and predicates, including nested guards. Both
// branches are considered: a read on either path retains the old state until
// that point. Unknown statements or calls invalidate the closed-phase proof.
class GuardedStateAccess final : public VNVisitorConst {
public:
    struct Access final {
        size_t m_reads = 0;
        size_t m_lastRead = 0;
        size_t m_writes = 0;
        std::vector<std::pair<size_t, AstAssign*>> m_assignments;
    };

private:
    std::map<AstVarScope*, Access> m_access;
    size_t m_order = 0;
    bool m_safe = true;

    void reject(const AstNode* nodep) {
        if (m_safe)
            V3Stats::addStatSum(
                std::string{"RepCut native direct-state, opaque "} + nodep->typeName(), 1);
        m_safe = false;
    }

    void visit(AstVarRef* nodep) override {
        Access& varAccess = m_access[nodep->varScopep()];
        if (nodep->access().isReadOrRW()) {
            ++varAccess.m_reads;
            varAccess.m_lastRead = m_order;
        }
        if (nodep->access().isWriteOrRW()) ++varAccess.m_writes;
    }
    void visit(AstAssign* nodep) override {
        ++m_order;
        if (const AstVarRef* const refp = VN_CAST(nodep->lhsp(), VarRef)) {
            m_access[refp->varScopep()].m_assignments.emplace_back(m_order, nodep);
        } else {
            reject(nodep);
        }
        iterateChildrenConst(nodep);
    }
    void visit(AstAssignW* nodep) override {
        // Source-backed combinational drivers retain their continuous-assignment
        // node kind until scheduling. Here they assign private phase temporaries
        // in sequence; count their accesses without treating them as candidates.
        ++m_order;
        iterateChildrenConst(nodep);
    }
    void visit(AstIf* nodep) override {
        ++m_order;
        iterateConst(nodep->condp());
        iterateAndNextConstNull(nodep->thensp());
        iterateAndNextConstNull(nodep->elsesp());
    }
    void visit(AstBegin* nodep) override { iterateChildrenConst(nodep); }
    void visit(AstComment*) override {}
    void visit(AstNodeCCall* nodep) override { reject(nodep); }
    void visit(AstNodeExpr* nodep) override {
        if (!nodep->isPure()) reject(nodep);
        iterateChildrenConst(nodep);
    }
    void visit(AstNode* nodep) override {
        if (VN_IS(nodep, NodeStmt)) reject(nodep);
        iterateChildrenConst(nodep);
    }

public:
    explicit GuardedStateAccess(const std::vector<AstNodeStmt*>& statements) {
        for (AstNodeStmt* const stmtp : statements) iterateConst(stmtp);
    }
    bool safe() const { return m_safe; }
    Access access(AstVarScope* vscp) const {
        const auto it = m_access.find(vscp);
        return it == m_access.end() ? Access{} : it->second;
    }
};

class GuardedStateRewrite final : public VNVisitor {
    const std::map<AstVarScope*, AstVarScope*>& m_released;

    void visit(AstAssign* nodep) override {
        AstVarRef* const refp = VN_CAST(nodep->lhsp(), VarRef);
        if (!refp) return;
        const auto found = m_released.find(refp->varScopep());
        if (found == m_released.end()) return;
        refp->varScopep(found->second);
        refp->varp(found->second->varp());
        // An initial candidate = old-state hold becomes an identity assignment;
        // ordinary constant/dead-code passes remove it after dependency lowering.
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    GuardedStateRewrite(const std::vector<AstNodeStmt*>& statements,
                        const std::map<AstVarScope*, AstVarScope*>& released)
        : m_released{released} {
        for (AstNodeStmt* const stmtp : statements) iterate(stmtp);
    }
};

class BlockLiveOutAnalysis final : public VNVisitorConst {
    static constexpr size_t NO_BLOCK = std::numeric_limits<size_t>::max();
    struct Access final {
        size_t m_block = NO_BLOCK;
        bool m_external = false;
    };
    std::map<AstVarScope*, Access> m_access;
    std::vector<std::set<AstVarScope*>> m_writes;
    size_t m_block = NO_BLOCK;

    void record(const AstVarRef* nodep) {
        AstVarScope* const vscp = nodep->varScopep();
        UASSERT_OBJ(vscp && !nodep->classOrPackagep(), nodep, "RepCut requires scoped references");
        Access& varAccess = m_access[vscp];
        if (m_block == NO_BLOCK) {
            varAccess.m_external = true;  // A guard read is outside all dataflow blocks.
        } else {
            if (varAccess.m_block == NO_BLOCK)
                varAccess.m_block = m_block;
            else if (varAccess.m_block != m_block)
                varAccess.m_external = true;
            if (nodep->access().isWriteOrRW()) m_writes[m_block].insert(vscp);
        }
    }
    void visit(AstIf* nodep) override {
        // This is traversal state: each branch boundary starts a fresh block.
        m_block = NO_BLOCK;
        iterateConst(nodep->condp());
        iterateAndNextConstNull(nodep->thensp());
        m_block = NO_BLOCK;
        iterateAndNextConstNull(nodep->elsesp());
        m_block = NO_BLOCK;
    }
    void visit(AstNodeStmt* nodep) override {
        if (m_block == NO_BLOCK) {
            m_block = m_writes.size();
            m_writes.emplace_back();
        }
        // Unsupported non-If statements remain atomic and will make the DFG
        // adapter decline the block. Do not discover extra blocks inside them.
        nodep->foreach([&](const AstVarRef* refp) { record(refp); });
    }
    void visit(AstVarRef* nodep) override { record(nodep); }
    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

public:
    explicit BlockLiveOutAnalysis(const std::vector<AstNodeStmt*>& statements) {
        for (AstNodeStmt* const stmtp : statements) iterateConst(stmtp);
    }
    std::vector<std::set<AstVarScope*>> result(const std::set<AstVarScope*>& preserve,
                                               bool report = true) const {
        std::vector<std::set<AstVarScope*>> liveOuts(m_writes.size());
        size_t locals = 0;
        size_t boundaries = 0;
        for (size_t i = 0; i < m_writes.size(); ++i) {
            for (AstVarScope* const vscp : m_writes[i]) {
                if (m_access.at(vscp).m_external || preserve.count(vscp)
                    || !vscp->varp()->isInternal() || vscp->varp()->isSigUserRdPublic()
                    || vscp->varp()->isSigUserRWPublic()) {
                    liveOuts[i].insert(vscp);
                    ++boundaries;
                } else {
                    ++locals;
                }
            }
        }
        if (report) {
            V3Stats::addStatSum("RepCut liveness, blocks", m_writes.size());
            V3Stats::addStatSum("RepCut liveness, private block definitions", locals);
            V3Stats::addStatSum("RepCut liveness, boundary definitions", boundaries);
        }
        return liveOuts;
    }
};

// Availability is a property of the current invocation and control path, not
// of a temporary's storage. Branches restore the incoming set, then retain only
// definitions made on both paths. The change log avoids copying the full set
// at each branch. Only newly available definitions participate in intersections.
class SelectionPreparation final : public VNVisitor {
    static constexpr size_t NO_BLOCK = std::numeric_limits<size_t>::max();
    std::set<AstVarScope*> m_available;
    std::vector<AstVarScope*> m_changes;
    std::set<AstVarScope*> m_blockWrites;
    std::vector<std::set<AstVarScope*>> m_inputs;
    size_t m_block = NO_BLOCK;
    size_t m_selections = 0;

    void define(AstVarScope* vscp) {
        if (m_available.insert(vscp).second) m_changes.push_back(vscp);
    }
    void restore(size_t checkpoint) {
        while (m_changes.size() > checkpoint) {
            m_available.erase(m_changes.back());
            m_changes.pop_back();
        }
    }
    void endBlock() {
        m_block = NO_BLOCK;
        m_blockWrites.clear();
    }
    void startBlock() {
        if (m_block != NO_BLOCK) return;
        m_block = m_inputs.size();
        m_inputs.emplace_back();
    }
    bool availableLeaf(const AstNodeExpr* nodep) const {
        if (VN_IS(nodep, Const)) return true;
        const AstVarRef* const refp = VN_CAST(nodep, VarRef);
        return refp && refp->access().isReadOnly() && !refp->classOrPackagep()
               && m_available.count(refp->varScopep());
    }
    AstAssign* selection(const AstIf* nodep) {
        const AstAssign* const thenp = VN_CAST(nodep->thensp(), Assign);
        const AstAssign* const elsep = VN_CAST(nodep->elsesp(), Assign);
        if (!thenp || !elsep || thenp->nextp() || elsep->nextp() || thenp->timingControlp()
            || elsep->timingControlp())
            return nullptr;
        const AstVarRef* const lhsp = VN_CAST(thenp->lhsp(), VarRef);
        const AstVarRef* const otherp = VN_CAST(elsep->lhsp(), VarRef);
        if (!lhsp || !otherp || !lhsp->access().isWriteOnly() || !otherp->access().isWriteOnly()
            || lhsp->classOrPackagep() || otherp->classOrPackagep()
            || lhsp->varScopep() != otherp->varScopep()
            || !lhsp->dtypep()->skipRefp()->isIntegralOrPacked())
            return nullptr;
        for (const AstNodeExpr* const armp : {thenp->rhsp(), elsep->rhsp()}) {
            if (!availableLeaf(armp) || armp->width() != lhsp->width()
                || armp->isSigned() != lhsp->isSigned())
                return nullptr;
        }
        AstCond* const condp
            = new AstCond{nodep->fileline(), nodep->condp()->cloneTree(false),
                          thenp->rhsp()->cloneTree(false), elsep->rhsp()->cloneTree(false)};
        condp->dtypep(lhsp->dtypep());
        ++m_selections;
        return new AstAssign{nodep->fileline(),
                             new AstVarRef{lhsp->fileline(), lhsp->varScopep(), VAccess::WRITE},
                             condp};
    }
    void visit(AstIf* nodep) override {
        if (AstAssign* const assignp = selection(nodep)) {
            nodep->replaceWith(assignp);
            pushDeletep(nodep);
            iterate(assignp);
            return;
        }
        // Like blockLiveOuts, traversal state deliberately ends at each If.
        endBlock();
        const size_t checkpoint = m_changes.size();
        iterateAndNextNull(nodep->thensp());
        const std::vector<AstVarScope*> thenValues{m_changes.begin() + checkpoint,
                                                   m_changes.end()};
        restore(checkpoint);
        endBlock();
        iterateAndNextNull(nodep->elsesp());
        std::vector<AstVarScope*> common;
        for (AstVarScope* const vscp : thenValues) {
            if (m_available.count(vscp)) common.push_back(vscp);
        }
        restore(checkpoint);
        for (AstVarScope* const vscp : common) define(vscp);
        endBlock();
    }
    void visit(AstAssign* nodep) override {
        startBlock();
        nodep->rhsp()->foreach([&](const AstCond* condp) {
            for (const AstNodeExpr* const armp : {condp->thenp(), condp->elsep()}) {
                const AstVarRef* const refp = VN_CAST(armp, VarRef);
                if (refp && availableLeaf(refp) && !m_blockWrites.count(refp->varScopep()))
                    m_inputs[m_block].insert(refp->varScopep());
            }
        });
        if (const AstVarRef* const refp = VN_CAST(nodep->lhsp(), VarRef)) {
            if (refp->access().isWriteOnly() && refp->varScopep() && !refp->classOrPackagep()
                && !nodep->timingControlp()) {
                define(refp->varScopep());
                m_blockWrites.insert(refp->varScopep());
            }
        }
    }
    void visit(AstNodeStmt*) override {
        // Opaque statements do not establish availability of their outputs.
        // The DFG adapter will reject this block without changing its AST.
        startBlock();
    }
    void visit(AstNode*) override {}

public:
    SelectionPreparation(const AstBegin* rootp, const std::set<AstVarScope*>& inputs)
        : m_available{inputs} {
        iterateAndNextNull(rootp->stmtsp());
    }
    const std::vector<std::set<AstVarScope*>>& inputs() const { return m_inputs; }
    size_t selections() const { return m_selections; }
};

class CopyGuards final : public VNVisitorConst {
    struct Context final {
        std::map<const AstNode*, std::array<size_t, 2>> m_edges;
        std::vector<size_t> m_children;
        size_t m_begin = 0;
        size_t m_end = 0;
    };
    std::map<AstVarScope*, Definition>& m_definitions;
    std::vector<Context> m_contexts{1};
    size_t m_guard = 0;

    size_t child(size_t parent, const AstNode* keyp, bool taken) {
        size_t& id = m_contexts[parent].m_edges[keyp][taken];
        if (id) return id;
        const size_t result = m_contexts.size();
        id = result;
        m_contexts[parent].m_children.push_back(result);
        m_contexts.emplace_back();
        return result;
    }
    void visit(AstIf* nodep) override {
        VL_RESTORER(m_guard);
        const size_t parent = m_guard;
        const AstNode* keyp = nodep;
        if (const AstVarRef* const refp = VN_CAST(nodep->condp(), VarRef)) {
            const Definition& def = m_definitions.at(refp->varScopep());
            // Separately emitted tests are equivalent only when the tested value
            // cannot change between them. Otherwise retain lexical identity.
            if (!def.m_writes
                || (def.m_writes == 1 && def.m_assignp && def.m_order < def.m_firstRead)) {
                keyp = refp->varScopep();
            }
        }
        iterateConst(nodep->condp());
        m_guard = child(parent, keyp, true);
        iterateAndNextConstNull(nodep->thensp());
        m_guard = child(parent, keyp, false);
        iterateAndNextConstNull(nodep->elsesp());
    }
    void visit(AstAssign* nodep) override {
        if (const AstVarRef* const refp = VN_CAST(nodep->lhsp(), VarRef)) {
            Definition& def = m_definitions.at(refp->varScopep());
            if (def.m_writes == 1 && def.m_assignp == nodep) def.m_guard = m_guard;
        }
        iterateChildrenConst(nodep);
    }
    void visit(AstVarRef* nodep) override {
        if (nodep->access().isReadOrRW()) {
            m_definitions.at(nodep->varScopep()).m_readGuards.push_back(m_guard);
        }
    }
    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

public:
    CopyGuards(AstBegin* rootp, std::map<AstVarScope*, Definition>& definitions)
        : m_definitions{definitions} {
        iterateConst(rootp);
        // Euler intervals make availability checks constant-time. Contexts are
        // interned by stable predicate and polarity, so reopened guards agree.
        std::vector<std::pair<size_t, bool>> pending{{0, false}};
        size_t order = 0;
        while (!pending.empty()) {
            const std::pair<size_t, bool> item = pending.back();
            pending.pop_back();
            Context& context = m_contexts[item.first];
            if (item.second) {
                context.m_end = order;
                continue;
            }
            context.m_begin = order++;
            pending.emplace_back(item.first, true);
            for (auto it = context.m_children.rbegin(); it != context.m_children.rend(); ++it) {
                pending.emplace_back(*it, false);
            }
        }
    }
    bool available(size_t definition, size_t use) const {
        return m_contexts[definition].m_begin <= m_contexts[use].m_begin
               && m_contexts[use].m_begin < m_contexts[definition].m_end;
    }
};

class CopyAnalysis final : public VNVisitorConst {
    std::map<AstVarScope*, Definition> m_definitions;
    std::vector<const AstAssign*> m_assignps;
    const AstAssign* m_assignp = nullptr;
    size_t m_order = 0;
    bool m_safe = true;
    std::unique_ptr<CopyGuards> m_guardsp;

    void visit(AstAssign* nodep) override {
        ++m_order;
        VL_RESTORER(m_assignp);
        m_assignp = nodep;
        if (nodep->timingControlp()) m_safe = false;
        m_assignps.push_back(nodep);
        iterateChildrenConst(nodep);
    }
    void visit(AstIf* nodep) override {
        ++m_order;
        iterateChildrenConst(nodep);
    }
    void visit(AstBegin* nodep) override { iterateChildrenConst(nodep); }
    void visit(AstNodeStmt* nodep) override {
        // In particular, loops and calls invalidate the single-execution proof.
        m_safe = false;
        iterateChildrenConst(nodep);
    }
    void visit(AstVarRef* nodep) override {
        if (!nodep->varScopep() || nodep->classOrPackagep()) {
            m_safe = false;
            return;
        }
        Definition& def = m_definitions[nodep->varScopep()];
        if (nodep->access().isReadOrRW()) def.m_firstRead = std::min(def.m_firstRead, m_order);
        if (nodep->access().isWriteOrRW()) {
            ++def.m_writes;
            def.m_order = m_order;
            def.m_assignp
                = m_assignp && m_assignp->lhsp() == nodep && nodep->access().isWriteOnly()
                      ? m_assignp
                      : nullptr;
        }
    }
    void visit(AstNodeExpr* nodep) override {
        if (!nodep->isPure()) m_safe = false;
        iterateChildrenConst(nodep);
    }
    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

public:
    explicit CopyAnalysis(AstBegin* rootp) {
        iterateConst(rootp);
        if (m_safe) m_guardsp.reset(new CopyGuards{rootp, m_definitions});
    }

    Aliases aliases(const std::set<AstVarScope*>& preserve) const {
        Aliases result;
        if (!m_safe) return result;
        // Statement order makes alias-chain resolution a single forward pass.
        for (const AstAssign* const assignp : m_assignps) {
            const AstVarRef* const lhsp = VN_CAST(assignp->lhsp(), VarRef);
            const AstNodeExpr* rhsp = assignp->rhsp();
            if (const AstRedOr* const redp = VN_CAST(rhsp, RedOr)) {
                if (redp->lhsp()->width() == 1) rhsp = redp->lhsp();
            }
            const AstVarRef* const refp = VN_CAST(rhsp, VarRef);
            if (!lhsp || !refp || !refp->access().isReadOnly()) continue;
            AstVarScope* const dstp = lhsp->varScopep();
            AstVarScope* const srcp = refp->varScopep();
            if (dstp == srcp || preserve.count(dstp) || !dstp->varp()->isInternal()
                || dstp->varp()->isSigUserRdPublic() || dstp->varp()->isSigUserRWPublic())
                continue;
            const AstNodeDType* const dtypep = lhsp->dtypep()->skipRefp();
            if (!dtypep->isIntegralOrPacked() || !dtypep->sameTree(refp->dtypep()->skipRefp()))
                continue;
            const Definition& dst = m_definitions.at(dstp);
            const Definition& src = m_definitions.at(srcp);
            if (dst.m_writes != 1 || dst.m_assignp != assignp || dst.m_firstRead <= dst.m_order)
                continue;
            if (src.m_writes
                && (src.m_writes != 1 || !src.m_assignp || src.m_order >= dst.m_order))
                continue;
            if (src.m_writes && !m_guardsp->available(src.m_guard, dst.m_guard)) continue;
            bool available = true;
            for (const size_t guard : dst.m_readGuards) {
                if (!m_guardsp->available(dst.m_guard, guard)) {
                    available = false;
                    break;
                }
            }
            if (!available) continue;
            const auto it = result.find(srcp);
            result.emplace(dstp, it == result.end() ? srcp : it->second);
        }
        return result;
    }
};

class CopyRewrite final : public VNVisitor {
    const Aliases& m_aliases;
    void visit(AstAssign* nodep) override {
        const AstVarRef* const lhsp = VN_CAST(nodep->lhsp(), VarRef);
        if (lhsp && m_aliases.count(lhsp->varScopep())) {
            pushDeletep(nodep->unlinkFrBack());
            return;
        }
        iterateChildren(nodep);
    }
    void visit(AstVarRef* nodep) override {
        if (!nodep->access().isReadOnly()) return;
        const auto it = m_aliases.find(nodep->varScopep());
        if (it == m_aliases.end()) return;
        nodep->varp(it->second->varp());
        nodep->varScopep(it->second);
    }
    void visit(AstIf* nodep) override {
        iterateChildren(nodep);
        if (!nodep->thensp() && !nodep->elsesp()) pushDeletep(nodep->unlinkFrBack());
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    CopyRewrite(AstBegin* rootp, const Aliases& aliases)
        : m_aliases{aliases} {
        iterate(rootp);
    }
};

// Only packed, explicitly read expressions can move. Calls, memory reads and
// opaque expression nodes end a block rather than relying on inferred aliases.
class LocalExpressionInputs final : public VNVisitorConst {
    bool m_safe = true;
    std::set<AstVarScope*> m_inputs;
    void visit(AstVarRef* nodep) override {
        if (!nodep->access().isReadOnly() || !nodep->varScopep() || nodep->classOrPackagep()
            || !nodep->dtypep()->skipRefp()->isIntegralOrPacked()) {
            m_safe = false;
        } else {
            m_inputs.insert(nodep->varScopep());
        }
    }
    void visit(AstConst*) override {}
    void visit(AstNodeUniop* nodep) override {
        if (!nodep->isPure()) m_safe = false;
        iterateChildrenConst(nodep);
    }
    void visit(AstNodeBiop* nodep) override {
        if (!nodep->isPure()) m_safe = false;
        iterateChildrenConst(nodep);
    }
    void visit(AstNodeTriop* nodep) override {
        if (!nodep->isPure()) m_safe = false;
        iterateChildrenConst(nodep);
    }
    void visit(AstNode*) override { m_safe = false; }

public:
    explicit LocalExpressionInputs(AstNodeExpr* nodep) { iterateConst(nodep); }
    bool safe() const { return m_safe; }
    const std::set<AstVarScope*>& inputs() const { return m_inputs; }
};

class LocalExpressionUses final : public VNVisitorConst {
public:
    struct Uses final {
        size_t m_reads = 0;
        size_t m_writes = 0;
    };

private:
    std::map<const AstVarScope*, Uses> m_uses;
    void visit(AstVarRef* nodep) override {
        if (!nodep->varScopep()) return;
        Uses& varUses = m_uses[nodep->varScopep()];
        if (nodep->access().isReadOrRW()) ++varUses.m_reads;
        if (nodep->access().isWriteOrRW()) ++varUses.m_writes;
    }
    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

public:
    explicit LocalExpressionUses(AstBegin* rootp) { iterateConst(rootp); }
    const Uses& uses(const AstVarScope* vscp) const { return m_uses.at(vscp); }
};

class LocalExpressionSubstitution final : public VNVisitor {
    struct Pending final {
        AstAssign* m_assignp = nullptr;
        size_t m_size = 0;
        std::map<AstVarScope*, size_t> m_versions;
    };
    const LocalExpressionUses& m_uses;
    const std::set<AstVarScope*>& m_preserve;
    const size_t m_limit;
    std::map<AstVarScope*, Pending> m_pending;
    std::map<AstVarScope*, size_t> m_versions;
    size_t m_order = 0;
    size_t m_rhsSize = 0;
    size_t m_substitutions = 0;

    void endBlock() { m_pending.clear(); }
    void visit(AstIf* nodep) override {
        // Predicates and both branch edges are hard boundaries. In particular,
        // a definition in an earlier reopened guard is not a local definition.
        endBlock();
        iterateAndNextNull(nodep->thensp());
        endBlock();
        iterateAndNextNull(nodep->elsesp());
        endBlock();
    }
    void visit(AstAssign* nodep) override {
        const AstVarRef* const lhsp = VN_CAST(nodep->lhsp(), VarRef);
        if (!lhsp || !lhsp->access().isWriteOnly() || !lhsp->varScopep() || lhsp->classOrPackagep()
            || nodep->timingControlp() || !lhsp->dtypep()->skipRefp()->isIntegralOrPacked()
            || !LocalExpressionInputs{nodep->rhsp()}.safe()) {
            endBlock();
            return;
        }
        VL_RESTORER(m_rhsSize);
        m_rhsSize = nodep->rhsp()->nodeCount();
        iterate(nodep->rhsp());
        AstVarScope* const dstp = lhsp->varScopep();
        m_versions[dstp] = ++m_order;
        const LocalExpressionUses::Uses& uses = m_uses.uses(dstp);
        if (uses.m_reads != 1 || uses.m_writes != 1 || m_rhsSize > m_limit
            || !dstp->varp()->isInternal() || !dstp->varp()->isTemp() || dstp->varp()->noSubst()
            || dstp->varp()->isSigUserRdPublic() || dstp->varp()->isSigUserRWPublic()
            || m_preserve.count(dstp)
            || !lhsp->dtypep()->skipRefp()->sameTree(nodep->rhsp()->dtypep()->skipRefp()))
            return;
        const LocalExpressionInputs inputs{nodep->rhsp()};
        // A self-reference captures the preceding value, not this definition.
        if (inputs.inputs().count(dstp)) return;
        Pending pending{nodep, m_rhsSize, {}};
        for (AstVarScope* const vscp : inputs.inputs())
            pending.m_versions.emplace(vscp, m_versions[vscp]);
        m_pending.emplace(dstp, std::move(pending));
    }
    void visit(AstVarRef* nodep) override {
        const auto it = m_pending.find(nodep->varScopep());
        if (it == m_pending.end()) return;
        const Pending& pending = it->second;
        if (m_rhsSize + pending.m_size - 1 > m_limit) return;
        for (const auto& version : pending.m_versions) {
            if (m_versions[version.first] != version.second) return;
        }
        AstAssign* const assignp = pending.m_assignp;
        AstNodeExpr* const exprp = assignp->rhsp();
        UASSERT_OBJ(nodep->access().isReadOnly(), nodep, "RepCut substitution requires a read");
        if (!nodep->dtypep()->skipRefp()->sameTree(exprp->dtypep()->skipRefp())) return;
        m_rhsSize += pending.m_size - 1;
        m_pending.erase(it);
        nodep->replaceWith(exprp->cloneTree(false));
        pushDeletep(nodep);
        pushDeletep(assignp->unlinkFrBack());
        ++m_substitutions;
    }
    void visit(AstNodeStmt*) override { endBlock(); }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    LocalExpressionSubstitution(const AstBegin* rootp, const LocalExpressionUses& uses,
                                const std::set<AstVarScope*>& preserve, size_t limit)
        : m_uses{uses}
        , m_preserve{preserve}
        , m_limit{limit} {
        iterateAndNextNull(rootp->stmtsp());
    }
    size_t substitutions() const { return m_substitutions; }
};

}  // namespace

size_t V3RepCut::substituteLocalExpressions(std::vector<AstNodeStmt*>& statements,
                                            const std::set<AstVarScope*>& preserve, size_t limit) {
    if (statements.empty()) return 0;
    AstBegin* const rootp = new AstBegin{statements.front()->fileline(), "", nullptr, false};
    for (AstNodeStmt* const stmtp : statements) rootp->addStmtsp(stmtp);
    const LocalExpressionUses uses{rootp};
    const size_t count = LocalExpressionSubstitution{rootp, uses, preserve, limit}.substitutions();
    statements.clear();
    while (AstNode* const nodep = rootp->stmtsp())
        statements.push_back(VN_AS(nodep->unlinkFrBack(), NodeStmt));
    rootp->deleteTree();
    return count;
}

std::vector<V3RepCut::DfgBlock> V3RepCut::prepareDfg(std::vector<AstNodeStmt*>& statements,
                                                     const std::set<AstVarScope*>& inputs,
                                                     const std::set<AstVarScope*>& preserve) {
    if (statements.empty()) return {};
    AstBegin* const rootp = new AstBegin{statements.front()->fileline(), "", nullptr, false};
    for (AstNodeStmt* const stmtp : statements) rootp->addStmtsp(stmtp);
    const SelectionPreparation preparation{rootp, inputs};
    statements.clear();
    while (AstNode* const nodep = rootp->stmtsp()) {
        statements.push_back(VN_AS(nodep->unlinkFrBack(), NodeStmt));
    }
    rootp->deleteTree();
    const std::vector<std::set<AstVarScope*>> liveOuts = blockLiveOuts(statements, preserve);
    UASSERT(liveOuts.size() == preparation.inputs().size(), "RepCut DFG block order changed");
    std::vector<DfgBlock> result;
    for (size_t i = 0; i < liveOuts.size(); ++i)
        result.push_back({liveOuts[i], preparation.inputs()[i]});
    V3Stats::addStatSum("RepCut DFG, available selections", preparation.selections());
    return result;
}

std::set<AstVarScope*>
V3RepCut::releaseStateCandidates(std::vector<AstNodeStmt*>& statements,
                                 const std::map<AstVarScope*, AstVarScope*>& destinations) {
    std::set<AstVarScope*> result;
    if (destinations.empty()) return result;
    const StateAccessVisitor analysis{statements};
    for (size_t i = 0; i < statements.size(); ++i) {
        AstAssign* const assignp = VN_CAST(statements[i], Assign);
        if (!assignp) continue;
        AstVarRef* const lhsp = VN_CAST(assignp->lhsp(), VarRef);
        if (!lhsp) continue;
        const auto it = destinations.find(lhsp->varScopep());
        if (it == destinations.end()) continue;
        AstVarScope* const candidatep = it->first;
        AstVarScope* const statep = it->second;
        const StateAccessVisitor::Access candidate = analysis.access(candidatep);
        const StateAccessVisitor::Access state = analysis.access(statep);
        if (candidate.m_reads || candidate.m_writes != 1 || state.m_writes
            || state.m_lastRead > i + 1 || candidatep->width() != statep->width()
            || candidatep->isSigned() != statep->isSigned())
            continue;
        // The RHS, including an old-state read in this assignment, is evaluated
        // before its LHS store. Later optimization must see this state mutation.
        lhsp->replaceWith(new AstVarRef{lhsp->fileline(), statep, VAccess::WRITE});
        lhsp->deleteTree();
        result.insert(candidatep);
    }
    return result;
}

std::vector<std::set<AstVarScope*>>
V3RepCut::blockLiveOuts(const std::vector<AstNodeStmt*>& statements,
                        const std::set<AstVarScope*>& preserve) {
    return BlockLiveOutAnalysis{statements}.result(preserve);
}

std::set<AstVarScope*>
V3RepCut::releaseGuardedStateCandidates(const std::vector<AstNodeStmt*>& statements,
                                        const std::map<AstVarScope*, AstVarScope*>& destinations) {
    std::set<AstVarScope*> result;
    if (destinations.empty()) return result;
    const GuardedStateAccess analysis{statements};
    if (!analysis.safe()) return result;
    std::map<AstVarScope*, AstVarScope*> released;
    for (const auto& pair : destinations) {
        AstVarScope* const candidatep = pair.first;
        AstVarScope* const statep = pair.second;
        const GuardedStateAccess::Access candidate = analysis.access(candidatep);
        const GuardedStateAccess::Access state = analysis.access(statep);
        if (candidate.m_reads || !candidate.m_writes
            || candidate.m_writes != candidate.m_assignments.size() || state.m_writes) {
            V3Stats::addStatSum("RepCut native direct-state, retained access", 1);
            continue;
        }
        if (!candidatep->dtypep()->skipRefp()->sameTree(statep->dtypep()->skipRefp())) {
            V3Stats::addStatSum("RepCut native direct-state, retained type", 1);
            continue;
        }
        bool safe = true;
        for (const auto& write : candidate.m_assignments) {
            const AstVarRef* const holdp = VN_CAST(write.second->rhsp(), VarRef);
            if (holdp && holdp->varScopep() == statep && holdp->access().isReadOnly()) continue;
            if (state.m_lastRead > write.first) {
                safe = false;
                break;
            }
        }
        if (!safe) {
            V3Stats::addStatSum("RepCut native direct-state, retained later read", 1);
            continue;
        }
        released.emplace(candidatep, statep);
        result.insert(candidatep);
    }
    GuardedStateRewrite{statements, released};
    return result;
}

size_t V3RepCut::eliminateCopies(std::vector<AstNodeStmt*>& statements,
                                 const std::set<AstVarScope*>& preserve) {
    if (statements.empty()) return 0;
    // Temporarily own the list so deletions can uniformly unlink root and nested
    // statements. This container never enters the model AST.
    AstBegin* const rootp = new AstBegin{statements.front()->fileline(), "", nullptr, false};
    for (AstNodeStmt* const stmtp : statements) rootp->addStmtsp(stmtp);
    const Aliases aliases = CopyAnalysis{rootp}.aliases(preserve);
    if (!aliases.empty()) CopyRewrite{rootp, aliases};
    statements.clear();
    while (AstNode* const nodep = rootp->stmtsp()) {
        statements.push_back(VN_AS(nodep->unlinkFrBack(), NodeStmt));
    }
    rootp->deleteTree();
    return aliases.size();
}

void V3RepCut::selfTestLocalExpressions(AstScope& scope) {
    FileLine* const flp = scope.fileline();
    std::vector<AstVarScope*> variables;
    const auto makeVar = [&](int width, VSigning sign = VSigning::UNSIGNED) {
        AstVarScope* const vscp
            = scope.createTemp("__Vrepcut_expression_test" + cvtToStr(variables.size()),
                               scope.findLogicDType(width, width, sign));
        vscp->varp()->isInternal(true);
        variables.push_back(vscp);
        return vscp;
    };
    const auto read = [&](AstVarScope* vscp) { return new AstVarRef{flp, vscp, VAccess::READ}; };
    const auto assign = [&](AstVarScope* dstp, AstNodeExpr* rhsp) {
        return new AstAssign{flp, new AstVarRef{flp, dstp, VAccess::WRITE}, rhsp};
    };
    AstVarScope* const enablep = makeVar(1);
    for (const int width : {7, 95}) {
        const VSigning sign = width == 95 ? VSigning::SIGNED : VSigning::UNSIGNED;
        AstVarScope* const ap = makeVar(width, sign);
        AstVarScope* const bp = makeVar(width, sign);
        AstVarScope* const tp = makeVar(width, sign);
        AstVarScope* const up = makeVar(width, sign);
        AstVarScope* const outp = makeVar(width, sign);
        std::vector<AstNodeStmt*> statements;
        const auto sum = [&]() { return new AstAdd{flp, read(ap), read(bp)}; };
        const auto check = [&](size_t expected, size_t limit = 32, bool keepTemp = false) {
            std::set<AstVarScope*> preserve{outp};
            if (keepTemp) preserve.insert(tp);
            UASSERT(substituteLocalExpressions(statements, preserve, limit) == expected,
                    "RepCut local expression availability or size proof");
            VNDeleter deleter;
            for (AstNodeStmt* const stmtp : statements) deleter.pushDeletep(stmtp);
            statements.clear();
        };
        statements = {assign(tp, sum()), assign(outp, read(tp))};
        check(1);
        statements = {assign(tp, sum()), assign(up, new AstXor{flp, read(tp), read(bp)}),
                      assign(outp, read(up))};
        check(2);
        statements = {assign(tp, sum()), assign(outp, read(tp))};
        check(0, 2);
        statements = {assign(tp, sum()), assign(outp, read(tp))};
        check(0, 32, true);
        // Direct state publication changes the value an earlier RHS captured.
        statements = {assign(tp, sum()), assign(ap, read(bp)), assign(outp, read(tp))};
        check(0);
        // The same source write in the consuming statement follows its RHS read.
        statements = {assign(tp, sum()), assign(ap, read(tp))};
        check(1);
        statements = {assign(tp, sum()), assign(outp, new AstXor{flp, read(tp), read(tp)})};
        check(0);
        statements = {assign(outp, read(tp)), assign(tp, sum())};
        check(0);
        statements = {assign(tp, sum()), assign(tp, read(bp)), assign(outp, read(tp))};
        check(0);
        statements = {assign(tp, sum()), new AstIf{flp, read(enablep), assign(outp, read(tp))}};
        check(0);
        statements = {new AstIf{flp, read(enablep), assign(tp, sum())},
                      new AstIf{flp, read(enablep), assign(outp, read(tp))}};
        check(0);
        AstAssign* const defp = assign(tp, sum());
        defp->addNext(assign(outp, read(tp)));
        statements = {new AstIf{flp, read(enablep), defp}};
        check(1);
        statements = {assign(tp, sum()), new AstCStmt{flp, "/* opaque barrier */"},
                      assign(outp, read(tp))};
        check(0);
        AstVarScope* const narrowp = makeVar(width - 1, sign);
        statements = {assign(narrowp, sum()), assign(outp, read(narrowp))};
        check(0);
    }
    VNDeleter deleter;
    for (AstVarScope* const vscp : variables) {
        AstVar* const varp = vscp->varp();
        deleter.pushDeletep(vscp->unlinkFrBack());
        deleter.pushDeletep(varp->unlinkFrBack());
    }
}

void V3RepCut::selfTestStateRelease(AstScope& scope) {
    FileLine* const flp = scope.fileline();
    std::vector<AstVarScope*> variables;
    const auto makeVar = [&](int width) {
        AstVarScope* const vscp
            = scope.createTemp("__Vrepcut_state_test" + cvtToStr(variables.size()),
                               scope.findLogicDType(width, width, VSigning::UNSIGNED));
        vscp->varp()->isInternal(true);
        variables.push_back(vscp);
        return vscp;
    };
    AstVarScope* const statep = makeVar(7);
    AstVarScope* const candidatep = makeVar(7);
    AstVarScope* const inputp = makeVar(7);
    AstVarScope* const outputp = makeVar(7);
    AstVarScope* const enablep = makeVar(1);
    AstVarScope* const widep = makeVar(95);
    AstVarScope* const wideCandidatep = makeVar(95);
    const auto read = [&](AstVarScope* vscp) { return new AstVarRef{flp, vscp, VAccess::READ}; };
    const auto assign = [&](AstVarScope* dstp, AstNodeExpr* rhsp) {
        return new AstAssign{flp, new AstVarRef{flp, dstp, VAccess::WRITE}, rhsp};
    };
    std::vector<AstNodeStmt*> statements;
    const auto clear = [&]() {
        VNDeleter deleter;
        for (AstNodeStmt* const stmtp : statements) deleter.pushDeletep(stmtp);
        statements.clear();
    };
    const std::map<AstVarScope*, AstVarScope*> destinations{{candidatep, statep}};
    const auto lhs = [](AstNodeStmt* stmtp) {
        return VN_AS(VN_AS(stmtp, Assign)->lhsp(), VarRef)->varScopep();
    };
    statements = {assign(candidatep, new AstAdd{flp, read(statep), read(inputp)})};
    UASSERT(releaseStateCandidates(statements, destinations) == std::set<AstVarScope*>{candidatep}
                && lhs(statements[0]) == statep,
            "RepCut missed a final old-state RHS read");
    clear();
    statements = {assign(candidatep, read(inputp)),
                  new AstIf{flp, read(enablep), assign(outputp, read(statep))}};
    UASSERT(releaseStateCandidates(statements, destinations).empty(),
            "RepCut published state before a later guarded read");
    clear();
    statements = {assign(candidatep, read(inputp)),
                  new AstIf{flp, new AstRedOr{flp, read(statep)}, assign(outputp, read(inputp))}};
    UASSERT(releaseStateCandidates(statements, destinations).empty(),
            "RepCut published state before a later predicate read");
    clear();
    statements = {assign(outputp, read(statep)), assign(candidatep, read(inputp))};
    UASSERT(releaseStateCandidates(statements, destinations).size() == 1,
            "RepCut missed a captured earlier state read");
    UASSERT(eliminateCopies(statements, {statep, outputp}) == 0 && statements.size() == 2,
            "RepCut lost an old-state snapshot after direct publication");
    clear();
    statements = {assign(candidatep, read(inputp)), assign(outputp, read(candidatep))};
    UASSERT(releaseStateCandidates(statements, destinations).empty(),
            "RepCut released a candidate still read by evaluation");
    clear();
    statements = {assign(statep, read(inputp)), assign(candidatep, read(inputp))};
    UASSERT(releaseStateCandidates(statements, destinations).empty(),
            "RepCut released a candidate with an existing state writer");
    clear();
    statements = {assign(candidatep, read(inputp)), assign(candidatep, read(statep))};
    UASSERT(releaseStateCandidates(statements, destinations).empty(),
            "RepCut released a multiply-written candidate");
    clear();
    statements = {assign(wideCandidatep, read(widep))};
    UASSERT(releaseStateCandidates(statements, {{wideCandidatep, widep}}).size() == 1
                && lhs(statements[0]) == widep,
            "RepCut failed to release a wide state candidate");
    clear();
    // A guarded write can retain its old-state hold without a private buffer.
    AstAssign* const guardedp = assign(candidatep, read(inputp));
    statements = {assign(candidatep, read(statep)), new AstIf{flp, read(enablep), guardedp}};
    UASSERT(releaseGuardedStateCandidates(statements, destinations).size() == 1
                && lhs(statements[0]) == statep && lhs(guardedp) == statep,
            "RepCut failed to release a guarded state candidate");
    clear();
    statements = {new AstIf{flp, read(enablep), assign(candidatep, read(inputp))},
                  assign(outputp, read(statep))};
    UASSERT(releaseGuardedStateCandidates(statements, destinations).empty(),
            "RepCut released state before a later unguarded read");
    clear();
    statements = {new AstIf{flp, read(enablep), assign(candidatep, read(inputp))},
                  new AstIf{flp, new AstRedOr{flp, read(statep)}, assign(outputp, read(inputp))}};
    UASSERT(releaseGuardedStateCandidates(statements, destinations).empty(),
            "RepCut released state before a later predicate read");
    clear();
    AstAssign* const snapshotp = assign(outputp, read(statep));
    AstAssign* const finalp = assign(candidatep, read(inputp));
    snapshotp->addNext(finalp);
    statements = {new AstIf{flp, read(enablep), snapshotp}};
    UASSERT(releaseGuardedStateCandidates(statements, destinations).size() == 1
                && lhs(finalp) == statep,
            "RepCut missed a last read inside a guard");
    clear();
    statements = {assign(candidatep, read(inputp)), assign(candidatep, read(statep))};
    UASSERT(releaseGuardedStateCandidates(statements, destinations).empty(),
            "RepCut changed an old-state restoration after an earlier candidate write");
    clear();
    statements = {assign(candidatep, read(statep)), assign(candidatep, read(inputp)),
                  new AstIf{flp, read(enablep), assign(candidatep, read(inputp))}};
    UASSERT(releaseGuardedStateCandidates(statements, destinations).size() == 1,
            "RepCut failed to preserve multiple writes after all old-state reads");
    clear();
    statements = {assign(candidatep, read(inputp)), assign(outputp, read(candidatep))};
    UASSERT(releaseGuardedStateCandidates(statements, destinations).empty(),
            "RepCut released a candidate with an internal reader");
    clear();
    statements = {assign(statep, read(inputp)), assign(candidatep, read(inputp))};
    UASSERT(releaseGuardedStateCandidates(statements, destinations).empty(),
            "RepCut released an already-written destination");
    clear();
    statements = {assign(candidatep, read(inputp)), new AstCStmt{flp, "/* observer */"}};
    UASSERT(releaseGuardedStateCandidates(statements, destinations).empty(),
            "RepCut released state across an opaque observer");
    clear();
    statements = {new AstIf{flp, read(enablep), assign(wideCandidatep, read(widep))}};
    UASSERT(releaseGuardedStateCandidates(statements, {{wideCandidatep, widep}}).size() == 1,
            "RepCut failed to release a guarded wide state candidate");
    clear();
    VNDeleter deleter;
    for (AstVarScope* const vscp : variables) {
        AstVar* const varp = vscp->varp();
        deleter.pushDeletep(vscp->unlinkFrBack());
        deleter.pushDeletep(varp->unlinkFrBack());
    }
}

void V3RepCut::selfTestCopies(AstScope& scope) {
    FileLine* const flp = scope.fileline();
    std::vector<AstVarScope*> variables;
    const auto makeVar = [&](int width, VSigning sign = VSigning::UNSIGNED) {
        AstVarScope* const vscp
            = scope.createTemp("__Vrepcut_copy_test" + cvtToStr(variables.size()),
                               scope.findLogicDType(width, width, sign));
        vscp->varp()->isInternal(true);
        vscp->varp()->noReset(true);
        variables.push_back(vscp);
        return vscp;
    };
    AstVarScope* const inputp = makeVar(7);
    AstVarScope* const ap = makeVar(7);
    AstVarScope* const bp = makeVar(7);
    AstVarScope* const outputp = makeVar(7);
    AstVarScope* const enablep = makeVar(1);
    AstVarScope* const decisionp = makeVar(1);
    AstVarScope* const signedp = makeVar(7, VSigning::SIGNED);
    const auto read = [&](AstVarScope* vscp) { return new AstVarRef{flp, vscp, VAccess::READ}; };
    const auto assign = [&](AstVarScope* dstp, AstNodeExpr* rhsp) {
        return new AstAssign{flp, new AstVarRef{flp, dstp, VAccess::WRITE}, rhsp};
    };
    std::vector<AstNodeStmt*> statements;
    const auto clear = [&]() {
        VNDeleter deleter;
        for (AstNodeStmt* const stmtp : statements) deleter.pushDeletep(stmtp);
        statements.clear();
    };
    const auto sourceOf
        = [](const AstAssign* nodep) { return VN_AS(nodep->rhsp(), VarRef)->varScopep(); };
    // The copied value is immutable across separately emitted guard segments.
    AstAssign* outp = assign(outputp, read(bp));
    AstIf* const guardedp = new AstIf{flp, read(enablep), outp};
    statements = {assign(ap, read(inputp)), new AstIf{flp, read(enablep), assign(bp, read(ap))},
                  guardedp};
    UASSERT(eliminateCopies(statements, {outputp}) == 2 && statements.size() == 1
                && statements[0] == guardedp && sourceOf(outp) == inputp,
            "RepCut copies did not preserve the guarded snapshot");
    clear();
    // A skipped definition cannot supply an unguarded read of an old value.
    outp = assign(outputp, read(ap));
    statements = {new AstIf{flp, read(enablep), assign(ap, read(inputp))}, outp};
    UASSERT(eliminateCopies(statements, {outputp}) == 0 && sourceOf(outp) == ap,
            "RepCut used a copy outside its defining guard");
    clear();
    // A source changed after a copy must not change the captured value.
    outp = assign(outputp, read(bp));
    statements = {assign(ap, read(inputp)), assign(bp, read(ap)),
                  assign(ap, new AstConst{flp, AstConst::WidthedValue{}, 7, 0x55}), outp};
    UASSERT(eliminateCopies(statements, {outputp}) == 0 && sourceOf(outp) == bp,
            "RepCut propagated a multiply-written source");
    clear();
    // Repeating a test after modifying its condition is not the same guard.
    outp = assign(outputp, read(ap));
    statements = {new AstIf{flp, read(enablep), assign(ap, read(inputp))},
                  assign(enablep, new AstConst{flp, AstConst::BitTrue{}}),
                  new AstIf{flp, read(enablep), outp}};
    UASSERT(eliminateCopies(statements, {outputp}) == 0 && sourceOf(outp) == ap,
            "RepCut conflated guards with a changing predicate");
    clear();
    // Even one write is unsafe when it happens after the snapshot.
    outp = assign(outputp, read(bp));
    statements = {assign(bp, read(ap)), assign(ap, read(inputp)), outp};
    UASSERT(eliminateCopies(statements, {outputp}) == 0 && sourceOf(outp) == bp,
            "RepCut propagated a future definition");
    clear();
    // An earlier read observes the previous invocation's value.
    outp = assign(outputp, read(ap));
    statements = {outp, assign(ap, read(inputp))};
    UASSERT(eliminateCopies(statements, {outputp}) == 0 && sourceOf(outp) == ap,
            "RepCut changed a read before its definition");
    clear();
    // Signedness changes are not identity copies.
    outp = assign(outputp, read(signedp));
    statements = {assign(signedp, read(inputp)), outp};
    UASSERT(eliminateCopies(statements, {outputp}) == 0 && sourceOf(outp) == signedp,
            "RepCut removed a type-changing copy");
    clear();
    AstIf* const branchp = new AstIf{flp, read(decisionp), assign(outputp, read(inputp))};
    statements = {assign(decisionp, new AstRedOr{flp, read(enablep)}), branchp};
    UASSERT(eliminateCopies(statements, {outputp}) == 1 && statements.size() == 1
                && VN_AS(branchp->condp(), VarRef)->varScopep() == enablep,
            "RepCut did not propagate a one-bit decision");
    clear();
    // Snapshot candidates retain their own storage even when they are copies.
    outp = assign(outputp, read(inputp));
    statements = {outp};
    UASSERT(eliminateCopies(statements, {outputp}) == 0 && statements[0] == outp,
            "RepCut eliminated an externally published snapshot");
    clear();
    // A private intermediate stays local; a value read under a later guard is
    // a block output, and publication remains live without an AST read here.
    statements = {assign(ap, new AstXor{flp, read(inputp),
                                        new AstConst{flp, AstConst::WidthedValue{}, 7, 0x35}}),
                  assign(bp, new AstAdd{flp, read(ap), read(ap)}),
                  new AstIf{flp, read(enablep), assign(outputp, read(bp))}};
    std::vector<std::set<AstVarScope*>> liveOuts
        = BlockLiveOutAnalysis{statements}.result({outputp}, false);
    UASSERT(liveOuts.size() == 2 && liveOuts[0] == std::set<AstVarScope*>{bp}
                && liveOuts[1] == std::set<AstVarScope*>{outputp},
            "RepCut liveness lost a cross-block or publication value");
    clear();
    statements = {assign(decisionp, read(enablep)),
                  new AstIf{flp, read(decisionp), assign(outputp, read(inputp))}};
    liveOuts = BlockLiveOutAnalysis{statements}.result({outputp}, false);
    UASSERT(liveOuts.size() == 2 && liveOuts[0] == std::set<AstVarScope*>{decisionp},
            "RepCut liveness omitted a guard read");
    clear();
    // Separate writers must not independently delete a shared variable binding.
    statements
        = {assign(ap, read(inputp)), new AstIf{flp, read(enablep), assign(ap, read(inputp))}};
    liveOuts = BlockLiveOutAnalysis{statements}.result({}, false);
    UASSERT(liveOuts.size() == 2 && liveOuts[0] == std::set<AstVarScope*>{ap}
                && liveOuts[1] == std::set<AstVarScope*>{ap},
            "RepCut liveness omitted an external writer");
    clear();
    VNDeleter deleter;
    for (AstVarScope* const vscp : variables) {
        AstVar* const varp = vscp->varp();
        deleter.pushDeletep(vscp->unlinkFrBack());
        deleter.pushDeletep(varp->unlinkFrBack());
    }
}

void V3RepCut::selfTestSelections(AstScope& scope) {
    FileLine* const flp = scope.fileline();
    V3DfgContext ctx;
    std::vector<AstVarScope*> variables;
    const auto makeVar = [&](int width, VSigning sign = VSigning::UNSIGNED) {
        AstVarScope* const vscp
            = scope.createTemp("__Vrepcut_selection_test" + cvtToStr(variables.size()),
                               scope.findLogicDType(width, width, sign));
        vscp->varp()->isInternal(true);
        variables.push_back(vscp);
        return vscp;
    };
    const auto read = [&](AstVarScope* vscp) { return new AstVarRef{flp, vscp, VAccess::READ}; };
    const auto assign = [&](AstVarScope* dstp, AstNodeExpr* rhsp) {
        return new AstAssign{flp, new AstVarRef{flp, dstp, VAccess::WRITE}, rhsp};
    };
    AstVarScope* const enablep = makeVar(1);
    std::vector<AstNodeStmt*> statements;
    const auto clear = [&]() {
        VNDeleter deleter;
        for (AstNodeStmt* const stmtp : statements) deleter.pushDeletep(stmtp);
        statements.clear();
    };
    const auto prepare = [&](const std::set<AstVarScope*>& inputs, size_t expected) {
        AstBegin* const rootp = new AstBegin{flp, "", nullptr, false};
        for (AstNodeStmt* const stmtp : statements) rootp->addStmtsp(stmtp);
        const SelectionPreparation preparation{rootp, inputs};
        UASSERT(preparation.selections() == expected, "RepCut selection availability proof");
        statements.clear();
        while (AstNode* const nodep = rootp->stmtsp())
            statements.push_back(VN_AS(nodep->unlinkFrBack(), NodeStmt));
        rootp->deleteTree();
        UASSERT(preparation.inputs().size()
                    == BlockLiveOutAnalysis{statements}.result({}, false).size(),
                "RepCut selection block enumeration");
        return preparation.inputs();
    };
    const auto representable = [&](const std::set<AstVarScope*>& inputs) {
        const VNUser1InUse user1InUse;
        bool result;
        {
            const std::unique_ptr<DfgGraph> dfgp
                = V3DfgPasses::astBlockToDfg(scope, statements, {}, inputs, ctx);
            result = static_cast<bool>(dfgp);
            if (dfgp) V3DfgPasses::typeCheck(*dfgp);
        }
        DfgDataType::reset();
        return result;
    };
    // The same proof must preserve non-word-aligned, wide and signed selections.
    for (const int width : {7, 95}) {
        const VSigning sign = width == 95 ? VSigning::SIGNED : VSigning::UNSIGNED;
        AstVarScope* const inputp = makeVar(width, sign);
        AstVarScope* const ap = makeVar(width, sign);
        AstVarScope* const bp = makeVar(width, sign);
        AstVarScope* const outputp = makeVar(width, sign);
        const auto select = [&](AstVarScope* dstp, AstNodeExpr* thenp, AstNodeExpr* elsep) {
            return new AstIf{flp, read(enablep), assign(dstp, thenp), assign(dstp, elsep)};
        };
        statements = {select(outputp, read(inputp), read(bp))};
        std::vector<std::set<AstVarScope*>> inputs = prepare({inputp, bp}, 1);
        UASSERT(inputs.size() == 1 && inputs[0] == (std::set<AstVarScope*>{inputp, bp})
                    && representable(inputs[0]) && !representable({}),
                "RepCut DFG admitted a selection without available inputs");
        clear();
        // A definition earlier in this block needs no external availability claim.
        statements = {assign(ap, new AstAdd{flp, read(inputp), read(bp)}),
                      select(outputp, read(ap), read(bp))};
        inputs = prepare({inputp, bp}, 1);
        UASSERT(inputs[0] == std::set<AstVarScope*>{bp} && representable(inputs[0]),
                "RepCut DFG lost an earlier local definition");
        clear();
        // A skipped definition must not expose stale storage, even under a
        // separately repeated test whose predicate might have changed.
        statements = {new AstIf{flp, read(enablep), assign(ap, read(inputp))},
                      assign(enablep, new AstConst{flp, AstConst::BitTrue{}}),
                      select(outputp, read(ap), read(bp))};
        prepare({inputp, bp, enablep}, 0);
        clear();
        // Lexically dominated uses are available, but an outside use is not.
        AstAssign* const definitionp = assign(ap, read(inputp));
        AstNode::addNext(static_cast<AstNodeStmt*>(definitionp),
                         select(outputp, read(ap), read(bp)));
        statements
            = {new AstIf{flp, read(enablep), definitionp}, select(outputp, read(ap), read(bp))};
        prepare({inputp, bp}, 1);
        UASSERT(VN_IS(statements.back(), If), "RepCut leaked a branch definition");
        clear();
        // A value defined by both paths is available after the join. The
        // computations themselves must stay in the original branch bodies.
        statements = {select(ap, new AstAdd{flp, read(inputp), read(bp)},
                             new AstSub{flp, read(inputp), read(bp)}),
                      select(outputp, read(ap), read(bp))};
        inputs = prepare({inputp, bp}, 1);
        UASSERT(VN_IS(statements.front(), If) && inputs.back().count(ap),
                "RepCut speculated guarded computation or lost join availability");
        clear();
        // A phase boundary is usable only with an explicit initialized-input proof.
        statements = {select(outputp, read(ap), read(bp))};
        prepare({bp}, 0);
        prepare({ap, bp}, 1);
        clear();
        // An input snapshot cannot be substituted with its later new value.
        statements = {select(outputp, read(ap), read(inputp)), assign(ap, read(inputp))};
        inputs = prepare({ap, inputp}, 1);
        UASSERT(!representable(inputs[0]), "RepCut DFG conflated old and new values");
        clear();
        statements = {assign(ap, read(inputp)), select(outputp, read(ap), read(bp)),
                      assign(ap, read(bp))};
        inputs = prepare({inputp, bp}, 1);
        UASSERT(!representable(inputs[0]), "RepCut DFG admitted repeated writers");
        clear();
        // The converter independently rejects computations in a Cond arm.
        AstCond* const condp
            = new AstCond{flp, read(enablep), new AstAdd{flp, read(inputp), read(bp)}, read(bp)};
        condp->dtypep(outputp->dtypep());
        statements = {assign(outputp, condp)};
        UASSERT(!representable({inputp, bp}), "RepCut DFG speculated a Cond arm");
        clear();
    }
    VNDeleter deleter;
    for (AstVarScope* const vscp : variables) {
        AstVar* const varp = vscp->varp();
        deleter.pushDeletep(vscp->unlinkFrBack());
        deleter.pushDeletep(varp->unlinkFrBack());
    }
}
