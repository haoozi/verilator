// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Partition-private keyed priority selection
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2026-2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//*************************************************************************
//
// Transpose repeated searches over the same ordered (enable, key, value)
// tuples into a private table. Reverse-priority writes preserve first-match
// behavior; a separately cleared valid table preserves each consumer's default.
// Only pure packed expressions over definitely available, single-definition
// values may be evaluated early. Groups never cross a statement-list region.
// Source if-chains and graph-lowered chains with private copy temporaries share
// the recognizer. Removed temporaries must have no outside references or live
// interface bindings. Discovery precedes rewriting; the partition graph and
// membership are untouched. Analysis and table dimensions have explicit bounds.
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3RepCutPriority.h"

#include "V3Hasher.h"
#include "V3Stats.h"

#include <map>
#include <memory>
#include <set>

VL_DEFINE_DEBUG_FUNCTIONS;

namespace V3RepCut {
namespace {
struct DeleteExpr final {
    void operator()(AstNodeExpr* nodep) const {
        if (nodep) nodep->deleteTree();
    }
};
using Expr = std::unique_ptr<AstNodeExpr, DeleteExpr>;
using Aliases = std::map<AstVarScope*, AstNodeExpr*>;

class PriorityLowering final : public VNVisitor {
    static constexpr size_t MIN_ARMS = 4;
    static constexpr size_t MIN_KEYS = 8;
    static constexpr size_t MAX_ARMS = 512;
    static constexpr int MAX_KEY_WIDTH = 10;
    static constexpr size_t MAX_EXPR_NODES = 128;
    struct Arm final {
        Expr m_key;
        Expr m_enable;
        Expr m_value;
    };
    struct Chain final {
        AstIf* m_rootp = nullptr;
        AstVarScope* m_targetp = nullptr;
        Expr m_default;
        uint32_t m_key = 0;
        bool m_hasKey = false;
        std::vector<Arm> m_arms;
    };
    struct Group final {
        std::vector<Chain> m_chains;
        std::set<uint32_t> m_keys;
    };
    AstScope& m_scope;
    const std::string& m_tag;
    const uint32_t m_storageGroup;
    const std::set<AstVarScope*>& m_preserve;
    std::map<AstVarScope*, size_t> m_writes;
    std::map<AstVarScope*, size_t> m_refs;
    Aliases m_definitions;
    Aliases m_chainDefinitions;
    std::set<AstVarScope*> m_available;
    std::vector<AstVarScope*> m_added;
    std::vector<Group> m_groups;
    size_t m_budget = 0;
    size_t m_tables = 0;
    size_t m_chains = 0;
    size_t m_arms = 0;

    void visit(AstNode* nodep) override { iterateChildren(nodep); }

    static AstVarRef* ref(AstVarScope* vscp, VAccess access = VAccess::READ) {
        return new AstVarRef{vscp->fileline(), vscp, access};
    }
    bool spend() {
        if (!m_budget) return false;
        --m_budget;
        return true;
    }
    bool stable(AstVarScope* vscp) const {
        const auto it = m_writes.find(vscp);
        return m_available.count(vscp) && (it == m_writes.end() || it->second <= 1);
    }
    // Only total, bounded-width arithmetic is speculated. Memory, calls,
    // division and opaque expression types retain their original guards.
    Expr expression(AstNodeExpr* nodep, const Aliases& aliases, size_t depth = 0) {
        if (!nodep || depth > 16 || !spend()) return {};
        size_t nodes = 0;
        const bool bad = nodep->exists([&](AstNode* childp) {
            return ++nodes > MAX_EXPR_NODES || !childp->isPure()
                   || !(VN_IS(childp, VarRef) || VN_IS(childp, Const) || VN_IS(childp, Sel)
                        || VN_IS(childp, And) || VN_IS(childp, Or) || VN_IS(childp, Xor)
                        || VN_IS(childp, Not) || VN_IS(childp, Add) || VN_IS(childp, Sub)
                        || VN_IS(childp, Extend) || VN_IS(childp, ExtendS) || VN_IS(childp, ShiftL)
                        || VN_IS(childp, ShiftR));
        });
        if (bad || nodep->width() > 64 || !nodep->dtypep()->skipRefp()->isIntegralOrPacked())
            return {};
        if (const AstVarRef* const varp = VN_CAST(nodep, VarRef)) {
            if (!varp->access().isReadOnly() || varp->classOrPackagep()) return {};
            const auto it = aliases.find(varp->varScopep());
            if (it != aliases.end()) return expression(it->second, aliases, depth + 1);
            const auto local = m_chainDefinitions.find(varp->varScopep());
            if (local != m_chainDefinitions.end())
                return expression(local->second, aliases, depth + 1);
            if (!stable(varp->varScopep())) return {};
            return Expr{nodep->cloneTree(false)};
        }
        Expr result{nodep->cloneTree(false)};
        std::vector<AstVarRef*> refs;
        result->foreach([&](AstVarRef* varp) { refs.push_back(varp); });
        for (AstVarRef* const varp : refs) {
            Expr replacement = expression(varp, aliases, depth + 1);
            if (!replacement) return {};
            varp->replaceWith(replacement.release());
            varp->deleteTree();
        }
        if (static_cast<size_t>(result->nodeCount()) > MAX_EXPR_NODES) return {};
        return result;
    }
    AstNodeExpr* resolve(AstNodeExpr* nodep) const {
        for (size_t i = 0; i < 16; ++i) {
            const AstVarRef* const refp = VN_CAST(nodep, VarRef);
            if (!refp) break;
            const auto local = m_chainDefinitions.find(refp->varScopep());
            if (local != m_chainDefinitions.end()) {
                nodep = local->second;
                continue;
            }
            if (!stable(refp->varScopep())) break;
            const auto it = m_definitions.find(refp->varScopep());
            if (it == m_definitions.end()) break;
            nodep = it->second;
        }
        return nodep;
    }
    bool key(AstNodeExpr* nodep, Expr& selector, uint32_t& value) {
        const AstEq* const eqp = VN_CAST(resolve(nodep), Eq);
        if (!eqp) return false;
        AstConst* cp = VN_CAST(eqp->lhsp(), Const);
        AstNodeExpr* inputp = eqp->rhsp();
        if (!cp) {
            cp = VN_CAST(eqp->rhsp(), Const);
            inputp = eqp->lhsp();
        }
        if (!cp || cp->num().isOpaque() || cp->num().isFourState() || cp->width() > 64)
            return false;
        uint64_t constant = cp->toUQuad();
        int lsb = 0;
        int width = inputp->width();
        if (const AstAnd* const andp = VN_CAST(inputp, And)) {
            AstConst* maskp = VN_CAST(andp->lhsp(), Const);
            AstNodeExpr* fromp = andp->rhsp();
            if (!maskp) {
                maskp = VN_CAST(andp->rhsp(), Const);
                fromp = andp->lhsp();
            }
            if (!maskp || maskp->num().isFourState() || maskp->width() > 64) return false;
            uint64_t mask = maskp->toUQuad();
            if (!mask || (constant & ~mask)) return false;
            while (!(mask & 1)) {
                ++lsb;
                mask >>= 1;
            }
            width = 0;
            while (mask & 1) {
                ++width;
                mask >>= 1;
            }
            if (mask) return false;
            constant >>= lsb;
            inputp = fromp;
        }
        if (width < 1 || width > MAX_KEY_WIDTH || constant >= (1ULL << width)) return false;
        selector = expression(inputp, {});
        if (!selector) return false;
        if (lsb || selector->width() != width)
            selector.reset(new AstSel{nodep->fileline(), selector.release(), lsb, width});
        value = constant;
        return true;
    }
    bool condition(AstNodeExpr* nodep, Arm& arm, uint32_t& value) {
        nodep = resolve(nodep);
        if (key(nodep, arm.m_key, value)) {
            arm.m_enable.reset(new AstConst{nodep->fileline(), AstConst::BitTrue{}});
            return true;
        }
        const AstAnd* const andp = VN_CAST(nodep, And);
        if (!andp || andp->width() != 1) return false;
        for (const bool left : {true, false}) {
            if (!key(left ? andp->lhsp() : andp->rhsp(), arm.m_key, value)) continue;
            arm.m_enable = expression(left ? andp->rhsp() : andp->lhsp(), {});
            return arm.m_enable && arm.m_enable->width() == 1;
        }
        return false;
    }
    bool leaf(AstNode* bodyp, AstVarScope*& targetp, Expr& value) {
        Aliases aliases;
        for (AstNode* stmtp = bodyp; stmtp; stmtp = stmtp->nextp()) {
            if (!spend()) return false;
            const AstAssign* const assignp = VN_CAST(stmtp, Assign);
            if (!assignp || assignp->timingControlp()) return false;
            const AstVarRef* const lhsp = VN_CAST(assignp->lhsp(), VarRef);
            if (!lhsp || !lhsp->access().isWriteOnly() || lhsp->classOrPackagep()) return false;
            if (!stmtp->nextp()) {
                targetp = lhsp->varScopep();
                value = expression(assignp->rhsp(), aliases);
                return value && value->width() == lhsp->width();
            }
            if (aliases.count(lhsp->varScopep())) return false;
            // Validate at the definition site, before later aliases exist.
            // Otherwise a forward read could acquire a later value.
            if (!expression(assignp->rhsp(), aliases)) return false;
            aliases.emplace(lhsp->varScopep(), assignp->rhsp());
        }
        return false;
    }
    bool chain(AstIf* nodep, Chain& result) {
        if (!spend() || result.m_arms.size() >= MAX_ARMS) return false;
        Arm arm;
        uint32_t value = 0;
        if (!condition(nodep->condp(), arm, value)) return false;
        if (result.m_hasKey && result.m_key != value) return false;
        result.m_key = value;
        result.m_hasKey = true;
        AstVarScope* targetp = nullptr;
        if (!leaf(nodep->thensp(), targetp, arm.m_value)) return false;
        if (result.m_targetp && result.m_targetp != targetp) return false;
        if (!result.m_targetp) result.m_targetp = targetp;
        // Graph joins may assign an inner temporary and copy it on return.
        result.m_arms.push_back(std::move(arm));
        AstNode* elsep = nodep->elsesp();
        if (!elsep) return true;
        // Graph lowering can define the next predicate within the else arm.
        // Resolve these private definitions into the same pure, available
        // inputs as the outer chain; never move their original statements.
        AstNode* nextp = elsep;
        while (VN_IS(nextp, Assign)) nextp = nextp->nextp();
        if (VN_IS(nextp, If)) {
            while (elsep != nextp) {
                if (!spend()) return false;
                const AstAssign* const assignp = VN_AS(elsep, Assign);
                const AstVarRef* const lhsp = VN_CAST(assignp->lhsp(), VarRef);
                if (!lhsp || assignp->timingControlp()
                    || assignp->rhsp()->exists([&](AstVarRef* refp) {
                           return !refp->access().isReadOnly()
                                  || (!stable(refp->varScopep())
                                      && !m_chainDefinitions.count(refp->varScopep()));
                       })
                    || !m_chainDefinitions.emplace(lhsp->varScopep(), assignp->rhsp()).second)
                    return false;
                elsep = elsep->nextp();
            }
        }
        if (AstIf* const nestedp = VN_CAST(elsep, If)) {
            AstNode* const tailp = nestedp->nextp();
            if (!tailp) {
                if (!chain(nestedp, result)) return false;
                return result.m_targetp == targetp;
            }
            const AstAssign* const copyp = VN_CAST(tailp, Assign);
            if (!copyp || tailp->nextp()) return false;
            const AstVarRef* const dstp = VN_CAST(copyp->lhsp(), VarRef);
            const AstVarRef* const srcp = VN_CAST(copyp->rhsp(), VarRef);
            if (!dstp || !srcp || dstp->varScopep() != targetp) return false;
            const size_t first = result.m_arms.size();
            result.m_targetp = nullptr;
            if (!chain(nestedp, result) || result.m_targetp != srcp->varScopep()) return false;
            for (size_t i = first; i < result.m_arms.size(); ++i)
                if (result.m_arms[i].m_value->width() != dstp->width()) return false;
            result.m_targetp = targetp;
            return true;
        }
        AstVarScope* defaultTargetp = nullptr;
        if (!leaf(elsep, defaultTargetp, result.m_default)) return false;
        return defaultTargetp == targetp;
    }
    bool privateIntermediates(const Chain& candidate) const {
        std::map<AstVarScope*, size_t> refs;
        std::set<AstVarScope*> written;
        candidate.m_rootp->foreach([&](AstVarRef* refp) {
            ++refs[refp->varScopep()];
            if (refp->access().isWriteOrRW()) written.insert(refp->varScopep());
        });
        // These sets and maps retain mutable AST pointers as lookup keys.
        // cppcheck-suppress constVariablePointer
        for (AstVarScope* const vscp : written) {
            if (vscp == candidate.m_targetp) continue;
            if (m_preserve.count(vscp) || !vscp->varp()->isInternal()
                || refs.at(vscp) != m_refs.at(vscp))
                return false;
        }
        return true;
    }
    static V3Hash hash(const Chain& chain) {
        V3Hash result{chain.m_targetp->dtypep()};
        for (const Arm& arm : chain.m_arms) {
            result += V3Hasher::uncachedHash(arm.m_key.get());
            result += V3Hasher::uncachedHash(arm.m_enable.get());
            result += V3Hasher::uncachedHash(arm.m_value.get());
        }
        return result;
    }
    static bool same(const Chain& a, const Chain& b) {
        if (a.m_arms.size() != b.m_arms.size() || a.m_targetp->dtypep() != b.m_targetp->dtypep())
            return false;
        for (size_t i = 0; i < a.m_arms.size(); ++i) {
            if (!a.m_arms[i].m_key->sameTree(b.m_arms[i].m_key.get())
                || !a.m_arms[i].m_enable->sameTree(b.m_arms[i].m_enable.get())
                || !a.m_arms[i].m_value->sameTree(b.m_arms[i].m_value.get()))
                return false;
        }
        return true;
    }
    void region(AstNode* bodyp) {
        const size_t checkpoint = m_added.size();
        std::map<V3Hash, size_t> groups;
        for (AstNode* stmtp = bodyp; stmtp; stmtp = stmtp->nextp()) {
            if (AstIf* const ifp = VN_CAST(stmtp, If)) {
                Chain candidate;
                candidate.m_rootp = ifp;
                m_chainDefinitions.clear();
                if (chain(ifp, candidate) && candidate.m_arms.size() >= MIN_ARMS
                    && privateIntermediates(candidate)) {
                    const V3Hash candidateHash = hash(candidate);
                    const auto it = groups.emplace(candidateHash, m_groups.size());
                    if (it.second) m_groups.emplace_back();
                    Group& group = m_groups[it.first->second];
                    if (group.m_chains.empty() || same(group.m_chains.front(), candidate)) {
                        group.m_keys.insert(candidate.m_key);
                        group.m_chains.push_back(std::move(candidate));
                    }
                } else {
                    region(ifp->thensp());
                    region(ifp->elsesp());
                }
            } else if (const AstNodeAssign* const assignp = VN_CAST(stmtp, NodeAssign)) {
                AstVarRef* const lhsp = VN_AS(assignp->lhsp(), VarRef);
                AstVarScope* const vscp = lhsp->varScopep();
                const bool stableOperands = !assignp->rhsp()->exists([&](AstVarRef* refp) {
                    return !refp->access().isReadOnly() || !stable(refp->varScopep());
                });
                if (m_writes.at(vscp) == 1 && m_available.insert(vscp).second) {
                    m_added.push_back(vscp);
                    if (stableOperands) m_definitions.emplace(vscp, assignp->rhsp());
                }
            }
        }
        while (m_added.size() > checkpoint) {
            m_definitions.erase(m_added.back());
            m_available.erase(m_added.back());
            m_added.pop_back();
        }
    }
    AstVarScope* table(const std::string& name, AstNodeDType* elementp, int entries) {
        FileLine* const flp = m_scope.fileline();
        AstNodeDType* const dtypep
            = new AstUnpackArrayDType{flp, elementp, new AstRange{flp, entries - 1, 0}};
        v3Global.rootp()->typeTablep()->addTypesp(dtypep);
        AstVarScope* const vscp = m_scope.createTemp(name, dtypep);
        vscp->varp()->isInternal(true);
        vscp->varp()->noReset(true);
        vscp->varp()->noSubst(true);
        vscp->varp()->trace(false);
        vscp->varp()->repcutStorageGroup(m_storageGroup);
        return vscp;
    }
    void lower(Group& group) {
        Chain& first = group.m_chains.front();
        int width = 0;
        for (const Arm& arm : first.m_arms) width = std::max(width, arm.m_key->width());
        const int entries = 1 << width;
        if (group.m_keys.size() < MIN_KEYS
            || first.m_arms.size() * group.m_keys.size() < entries + 4 * first.m_arms.size())
            return;
        FileLine* const flp = first.m_rootp->fileline();
        const std::string name = m_tag + "__priority" + cvtToStr(m_tables++);
        AstVarScope* const validp
            = table(name + "_valid", m_scope.findBitDType(1, 1, VSigning::UNSIGNED), entries);
        AstVarScope* const valuep = table(name + "_value", first.m_targetp->dtypep(), entries);
        AstCMethodHard* const fillp
            = new AstCMethodHard{flp, ref(validp, VAccess::WRITE), VCMethod::UNPACKED_FILL};
        fillp->addPinsp(new AstConst{flp, AstConst::BitFalse{}});
        fillp->dtypeSetVoid();
        AstNodeStmt* const setup = fillp->makeStmt();
        for (auto it = first.m_arms.rbegin(); it != first.m_arms.rend(); ++it) {
            AstAssign* const storep = new AstAssign{
                flp,
                new AstArraySel{flp, ref(valuep, VAccess::WRITE), it->m_key->cloneTree(false)},
                it->m_value->cloneTree(false)};
            storep->addNext(new AstAssign{
                flp,
                new AstArraySel{flp, ref(validp, VAccess::WRITE), it->m_key->cloneTree(false)},
                new AstConst{flp, AstConst::BitTrue{}}});
            setup->addNext(new AstIf{flp, it->m_enable->cloneTree(false), storep});
        }
        first.m_rootp->addHereThisAsNext(setup);
        for (Chain& candidate : group.m_chains) {
            AstNodeStmt* elsep = nullptr;
            if (candidate.m_default)
                elsep = new AstAssign{flp, ref(candidate.m_targetp, VAccess::WRITE),
                                      candidate.m_default.release()};
            AstIf* const replacementp = new AstIf{
                flp, new AstArraySel{flp, ref(validp), new AstConst{flp, candidate.m_key}},
                new AstAssign{
                    flp, ref(candidate.m_targetp, VAccess::WRITE),
                    new AstArraySel{flp, ref(valuep), new AstConst{flp, candidate.m_key}}},
                elsep};
            candidate.m_rootp->replaceWith(replacementp);
            pushDeletep(candidate.m_rootp);
            ++m_chains;
            m_arms += candidate.m_arms.size();
        }
    }

public:
    PriorityLowering(AstScope& scope, AstNode* bodyp, const std::set<AstVarScope*>& inputs,
                     const std::set<AstVarScope*>& preserve, const std::string& tag,
                     uint32_t storageGroup)
        : m_scope{scope}
        , m_tag{tag}
        , m_storageGroup{storageGroup}
        , m_preserve{preserve} {
        for (AstNode* stmtp = bodyp; stmtp; stmtp = stmtp->nextp()) {
            m_budget += 8 * stmtp->nodeCount();
            stmtp->foreach([&](AstVarRef* refp) {
                ++m_refs[refp->varScopep()];
                if (refp->access().isWriteOrRW()) ++m_writes[refp->varScopep()];
            });
        }
        for (AstVarScope* const vscp : inputs)
            if (!m_writes.count(vscp)) m_available.insert(vscp);
        region(bodyp);
    }
    void apply() {
        for (Group& group : m_groups) lower(group);
        V3Stats::addStatSum("RepCut priority, private tables", m_tables);
        V3Stats::addStatSum("RepCut priority, chains lowered", m_chains);
        V3Stats::addStatSum("RepCut priority, arms lowered", m_arms);
        UINFO(3, m_tag << ": priority tables=" << m_tables << " chains=" << m_chains);
    }
};
}  //namespace

void lowerPrioritySelections(AstScope& scope, std::vector<AstNodeStmt*>& statements,
                             const std::set<AstVarScope*>& inputs,
                             const std::set<AstVarScope*>& preserve, const std::string& tag,
                             uint32_t storageGroup) {
    if (statements.empty()) return;
    for (AstNodeStmt* const stmtp : statements) {
        AstNode* opaquep = nullptr;
        if (stmtp->exists([&](AstNode* nodep) {
                opaquep = nodep;
                if (const AstNodeAssign* const assignp = VN_CAST(nodep, NodeAssign))
                    return (!VN_IS(nodep, Assign) && !VN_IS(nodep, AssignW))
                           || !VN_IS(assignp->lhsp(), VarRef) || assignp->timingControlp();
                if (VN_IS(nodep, NodeStmt)) return !VN_IS(nodep, If);
                return !nodep->isPure();
            })) {
            UINFO(3, tag << ": opaque priority statement: " << opaquep->typeName());
            return;
        }
    }
    AstBegin* const blockp = new AstBegin{scope.fileline(), "", nullptr, true};
    for (AstNodeStmt* const stmtp : statements) blockp->addStmtsp(stmtp);
    PriorityLowering lowering{scope, blockp->stmtsp(), inputs, preserve, tag, storageGroup};
    lowering.apply();
    statements.clear();
    while (AstNode* const stmtp = blockp->stmtsp())
        statements.push_back(VN_AS(stmtp->unlinkFrBack(), NodeStmt));
    blockp->deleteTree();
}
}  //namespace V3RepCut
