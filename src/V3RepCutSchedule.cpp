// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Batch independent RepCut kernels using native triggers
//
// Code available from: https://verilator.org
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2026-2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//*************************************************************************
// Combine certified kernels only after V3Sched has replaced their source
// sensitivities with native trigger expressions. The emitter has already proved
// that their computation is independent and staged values that another worker,
// domain or retained procedure can still read. This pass consumes that proof;
// it must not infer independence merely because calls use different clocks.
//
// A union of triggers starts one fixed dispatch. Inside each worker, every
// original kernel keeps its own trigger guard. Shared pure helpers run before
// their consumers under the union of just those consumers' triggers. Their
// values are valid for this invocation only; nothing is cached across eval calls.
// Helpers and dispatchers remain bounded sequential compilation chunks.
//
// Joining computation does not permit publication under the dispatch's broader
// trigger union. Preserve the original guards on NBA captures and ordered
// callback/effect bodies, and describe writes with domain-specific POST markers
// that depend on dispatch completion. These analysis dependencies let native
// ordering place publication and settling in their original event regions.
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3ExecGraph.h"
#include "V3RepCut.h"
#include "V3Sched.h"
#include "V3Stats.h"

#include <map>
#include <set>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

namespace V3RepCut {
namespace {
class NativeBatch final {
    struct Kernel final {
        AstCCall* m_callp;
        AstSenTree* m_senp;
        AstCFunc* m_dispatchp;
        AstExecGraph* m_graphp;
    };
    struct Shared final {
        AstCFunc* m_funcp = nullptr;
        std::vector<const Kernel*> m_kernels;
    };
    AstNetlist* const m_netlistp;
    V3Order::TrigToSenMap& m_trigToSen;
    const size_t m_limit
        = std::max(32, v3Global.opt.outputSplitCFuncs() ? v3Global.opt.outputSplitCFuncs()
                                                        : V3Options::REPCUT_DEFAULT_CFUNCS);
    size_t m_ordinal = 0;
    size_t m_functions = 0;
    size_t m_domainWrites = 0;

    static std::vector<AstCFunc*> takeShared(AstCFunc* rootp) {
        std::vector<AstCFunc*> pending{rootp};
        std::set<AstCFunc*> visited;
        std::map<uint32_t, AstCFunc*> helpers;
        while (!pending.empty()) {
            AstCFunc* const funcp = pending.back();
            pending.pop_back();
            if (!visited.insert(funcp).second) continue;
            std::vector<AstStmtExpr*> remove;
            funcp->foreach([&](AstCCall* callp) {
                AstCFunc* const targetp = callp->funcp();
                if (targetp->repcutSharedOrder()) {
                    UASSERT_OBJ(!callp->argsp(), callp, "Shared native helper has arguments");
                    helpers.emplace(targetp->repcutSharedOrder(), targetp);
                    remove.push_back(VN_AS(callp->backp(), StmtExpr));
                } else if (targetp->isRepCut()) {
                    pending.push_back(targetp);
                }
            });
            for (AstStmtExpr* const stmtp : remove)
                VL_DO_DANGLING(stmtp->unlinkFrBack()->deleteTree(), stmtp);
            V3Stats::addStatSum("RepCut shared domains, removed helper calls", remove.size());
        }
        std::vector<AstCFunc*> result;
        for (const auto& helper : helpers) result.push_back(helper.second);
        return result;
    }

    static AstNodeStmt* call(AstCFunc* funcp) {
        AstCCall* const callp = new AstCCall{funcp->fileline(), funcp};
        callp->dtypeSetVoid();
        return callp->makeStmt();
    }
    AstCFunc* function(AstScope* scopep, const std::string& tag,
                       const std::vector<AstNodeStmt*>& statements) {
        AstCFunc* const funcp = new AstCFunc{scopep->fileline(), tag, scopep};
        funcp->isLoose(true);
        funcp->isStatic(false);
        funcp->isConst(false);
        funcp->dontCombine(true);
        funcp->noInline(true);
        funcp->isRepCut(true);
        for (AstNodeStmt* const stmtp : statements) funcp->addStmtsp(stmtp);
        scopep->addBlocksp(funcp);
        ++m_functions;
        return funcp;
    }
    AstCFunc* chunks(AstScope* scopep, const std::string& tag,
                     const std::vector<AstNodeStmt*>& statements) {
        std::vector<AstNodeStmt*> chunk;
        std::vector<AstNodeStmt*> calls;
        size_t size = 0;
        size_t ordinal = 0;
        for (AstNodeStmt* const stmtp : statements) {
            const size_t nodes = stmtp->nodeCount();
            UASSERT_OBJ(nodes <= m_limit, stmtp, "Native batch guard exceeds chunk budget");
            if (size + nodes > m_limit) {
                calls.push_back(call(function(scopep, tag + "_c" + cvtToStr(ordinal++), chunk)));
                chunk.clear();
                size = 0;
            }
            chunk.push_back(stmtp);
            size += nodes;
        }
        if (calls.empty()) return function(scopep, tag, chunk);
        if (!chunk.empty())
            calls.push_back(call(function(scopep, tag + "_c" + cvtToStr(ordinal), chunk)));
        return chunks(scopep, tag, calls);
    }
    static AstNodeExpr* trigger(const Kernel& kernel) {
        AstNodeExpr* resultp = nullptr;
        for (AstSenItem* itemp = kernel.m_senp->sensesp(); itemp;
             itemp = VN_AS(itemp->nextp(), SenItem)) {
            UASSERT_OBJ(itemp->edgeType() == VEdgeType::ET_TRUE && !itemp->condp(), itemp,
                        "Native batch needs remapped scheduler triggers");
            AstNodeExpr* const bitp
                = new AstRedOr{itemp->fileline(), itemp->sensp()->cloneTree(false)};
            resultp = resultp ? new AstLogOr{itemp->fileline(), resultp, bitp} : bitp;
        }
        return resultp;
    }
    void sharedGuard(AstScope* scopep, const Shared& shared, const std::string& tag,
                     uint32_t thread, std::vector<AstNodeStmt*>& statements) {
        FileLine* const flp = scopep->fileline();
        AstNodeExpr* guardp = nullptr;
        for (const Kernel* const kernelp : shared.m_kernels)
            guardp = guardp ? new AstLogOr{flp, guardp, trigger(*kernelp)} : trigger(*kernelp);
        if (static_cast<size_t>(guardp->nodeCount()) + 3 > m_limit) {
            // Keep a large domain union bounded even with a tiny chunk budget.
            // This temporary belongs to the same fixed worker as the helpers.
            guardp->deleteTree();
            AstVarScope* const activep = scopep->createTemp(tag, 1);
            activep->varp()->noReset(true);
            activep->varp()->trace(false);
            activep->varp()->isInternal(true);
            activep->varp()->repcutStorageGroup(
                storageGroup((thread + 1) % v3Global.opt.threads(), StorageRole::PRIVATE_EVAL));
            const auto ref = [&](VAccess access = VAccess::READ) {
                return new AstVarRef{flp, activep, access};
            };
            statements.push_back(
                new AstAssign{flp, ref(VAccess::WRITE), new AstConst{flp, AstConst::BitFalse{}}});
            for (const Kernel* const kernelp : shared.m_kernels)
                statements.push_back(new AstAssign{flp, ref(VAccess::WRITE),
                                                   new AstOr{flp, ref(), trigger(*kernelp)}});
            guardp = ref();
        }
        statements.push_back(new AstIf{flp, guardp, call(shared.m_funcp)});
    }
    static void dependency(AstCCall* callp, AstVarScope* vscp, bool output) {
        FileLine* const flp = callp->fileline();
        AstVar* const argp = new AstVar{flp, VVarType::BLOCKTEMP, "__Vcompletion", vscp->dtypep()};
        argp->funcLocal(true);
        argp->direction(output ? VDirection::OUTPUT : VDirection::CONSTREF);
        argp->isConst(!output);
        callp->funcp()->addArgsp(argp);
        callp->addArgsp(new AstVarRef{flp, vscp, output ? VAccess::WRITE : VAccess::READ});
    }
    void domainDependencies(AstScope* scopep, const Kernel& kernel, AstCCall* batchp,
                            AstVarScope* completep, const std::string& tag,
                            V3Sched::LogicByScope& logic, size_t& ordinal) {
        // The dispatch runs under the union of all clocks, but its writes only
        // occur on their original clock. Expose each domain's writes in a POST
        // marker that depends on dispatch completion. This orders settling after
        // the join without broadening its sensitivity to unrelated clocks.
        AstCFunc* const markerp = function(scopep, tag, {});
        markerp->repcutDependencyArgs(true);
        AstCCall* const callp = new AstCCall{scopep->fileline(), markerp};
        callp->dtypeSetVoid();
        dependency(callp, completep, false);
        AstNode* formalp = kernel.m_callp->funcp()->argsp();
        for (AstNode* actualp = kernel.m_callp->argsp(); actualp;
             actualp = actualp->nextp(), formalp = formalp->nextp()) {
            AstVarRef* const refp = VN_AS(actualp, VarRef);
            UASSERT_OBJ(!refp->access().isRW(), actualp, "Mixed native dependency operand");
            AstCCall* const destp = refp->access().isWriteOnly() ? callp : batchp;
            AstVar* const argp = VN_AS(formalp, Var)->cloneTree(false);
            argp->name("__Vdependency" + cvtToStr(ordinal++));
            destp->funcp()->addArgsp(argp);
            destp->addArgsp(refp->cloneTree(false));
            if (refp->access().isWriteOnly()) ++m_domainWrites;
        }
        AstAlwaysPost* const postp = new AstAlwaysPost{scopep->fileline()};
        postp->addStmtsp(callp->makeStmt());
        logic.add(scopep, kernel.m_senp, postp);
    }
    void combine(AstScope* scopep, const std::vector<Kernel>& kernels,
                 V3Sched::LogicByScope& logic) {
        FileLine* const flp = scopep->fileline();
        const std::string tag = "__Vrepcut_batch" + cvtToStr(m_ordinal++);
        AstExecGraph* const graphp = new AstExecGraph{flp, tag};
        std::map<uint32_t, std::vector<AstNodeStmt*>> workers;
        std::map<uint32_t, std::map<uint32_t, Shared>> shared;
        std::map<uint32_t, AstNodeExpr*> active;
        AstSenTree* const senp = new AstSenTree{flp, nullptr};
        AstSenTree* const originalp = new AstSenTree{flp, nullptr};
        for (const Kernel& kernel : kernels) {
            senp->addSensesp(kernel.m_senp->sensesp()->cloneTree(true));
            originalp->addSensesp(m_trigToSen.at(kernel.m_senp)->sensesp()->cloneTree(true));
            for (V3GraphVertex& vertex : kernel.m_graphp->depGraphp()->vertices()) {
                const ExecMTask* const taskp = vertex.as<ExecMTask>();
                UASSERT_OBJ(taskp->hasFixedThread() && taskp->inEmpty() && taskp->outEmpty(),
                            graphp, "Dependent task in closed native batch");
                const std::vector<AstCFunc*> prefixes = v3Global.opt.repcutShareDomains()
                                                            ? takeShared(taskp->funcp())
                                                            : std::vector<AstCFunc*>{};
                for (AstCFunc* const funcp : prefixes) {
                    Shared& prefix = shared[taskp->fixedThread()][funcp->repcutSharedOrder()];
                    UASSERT_OBJ(!prefix.m_funcp || prefix.m_funcp == funcp, funcp,
                                "Shared helper order collision");
                    prefix.m_funcp = funcp;
                    prefix.m_kernels.push_back(&kernel);
                }
                workers[taskp->fixedThread()].push_back(
                    new AstIf{flp, trigger(kernel), call(taskp->funcp())});
                AstNodeExpr*& guardp = active[taskp->fixedThread()];
                guardp = guardp ? new AstLogOr{flp, guardp, trigger(kernel)} : trigger(kernel);
            }
        }
        // Retain these expressions as AST children through optimization and
        // descoping. ExecGraph lowers them to caller-local guards before launch.
        for (int thread = 0; thread < v3Global.opt.threads(); ++thread) {
            AstNodeExpr* const guardp = active[thread];
            graphp->addActivep(guardp ? guardp : new AstConst{flp, AstConst::BitFalse{}});
        }
        for (const auto& worker : workers) {
            std::vector<AstNodeStmt*> statements;
            for (const auto& prefix : shared[worker.first]) {
                sharedGuard(scopep, prefix.second, tag + "_shared" + cvtToStr(prefix.first),
                            worker.first, statements);
                V3Stats::addStatSum("RepCut shared domains, factored helpers", 1);
            }
            statements.insert(statements.end(), worker.second.begin(), worker.second.end());
            AstCFunc* const bodyp
                = chunks(scopep, tag + "_w" + cvtToStr(worker.first), statements);
            ExecMTask* const taskp = new ExecMTask{graphp, scopep, call(bodyp)};
            taskp->funcp()->isRepCut(true);
            taskp->fixedThread(worker.first);
            taskp->cost(1);
            taskp->priority(1);
            graphp->addStmtsp(call(taskp->funcp()));
        }
        AstCFunc* const funcp = function(scopep, tag, {graphp});
        funcp->repcutDependencyArgs(true);
        AstCCall* const callp = new AstCCall{flp, funcp};
        callp->dtypeSetVoid();
        AstVarScope* const completep = scopep->createTemp(tag + "_complete", 1);
        dependency(callp, completep, true);
        AstAlways* const alwaysp
            = new AstAlways{flp, VAlwaysKwd::ALWAYS, nullptr, callp->makeStmt()};
        size_t ordinal = 0;
        size_t domain = 0;
        for (const Kernel& kernel : kernels) {
            domainDependencies(scopep, kernel, callp, completep,
                               tag + "_domain" + cvtToStr(domain++), logic, ordinal);
            // Leave any other references valid. Dead removes these old wrappers
            // once all callers have moved; worker bodies are reused above.
            kernel.m_dispatchp->entryPoint(false);
            kernel.m_callp->funcp()->repcutNativeBatch(false);
            AstStmtExpr* const stmtp = VN_AS(kernel.m_callp->backp(), StmtExpr);
            AstNodeProcedure* const procp = VN_AS(stmtp->backp(), NodeProcedure);
            // NBA captures and callback/effect bodies originally followed the
            // compute call in the same process. Clocked reads alone do not
            // create read-after-write edges, so retain this sequencing and
            // each original domain guard explicitly on the caller.
            if (AstNode* const tailp = stmtp->nextp()) {
                tailp->unlinkFrBackWithNext();
                alwaysp->addStmtsp(new AstIf{flp, trigger(kernel), tailp});
            }
            VL_DO_DANGLING(stmtp->unlinkFrBack()->deleteTree(), stmtp);
            if (!procp->stmtsp()) VL_DO_DANGLING(procp->unlinkFrBack()->deleteTree(), procp);
        }
        m_netlistp->topScopep()->addSenTreesp(senp);
        m_netlistp->topScopep()->addSenTreesp(originalp);
        m_trigToSen.emplace(senp, originalp);
        logic.add(scopep, senp, alwaysp);
        V3Stats::addStatSum("RepCut native batch, domains", kernels.size());
        V3Stats::addStatSum("RepCut native batch, dispatches", 1);
    }

public:
    NativeBatch(AstNetlist* netlistp, const std::vector<V3Sched::LogicByScope*>& logic,
                V3Order::TrigToSenMap& trigToSen)
        : m_netlistp{netlistp}
        , m_trigToSen{trigToSen} {
        std::map<AstScope*, std::vector<Kernel>> scopes;
        for (const V3Sched::LogicByScope* const logicp : logic) {
            for (const auto& pair : *logicp) {
                pair.second->foreach([&](AstCCall* callp) {
                    if (!callp->funcp()->repcutNativeBatch()) return;
                    const AstStmtExpr* const stmtp = VN_CAST(callp->funcp()->stmtsp(), StmtExpr);
                    const AstCCall* const dispatchp
                        = stmtp ? VN_CAST(stmtp->exprp(), CCall) : nullptr;
                    AstExecGraph* const graphp
                        = dispatchp ? VN_CAST(dispatchp->funcp()->stmtsp(), ExecGraph) : nullptr;
                    if (!graphp || stmtp->nextp() || graphp->nextp()) return;
                    scopes[pair.first].push_back(
                        {callp, pair.second->sentreep(), dispatchp->funcp(), graphp});
                });
            }
        }
        for (const auto& scope : scopes) {
            if (scope.second.size() > 1) combine(scope.first, scope.second, *logic.front());
        }
        V3Stats::addStatSum("RepCut native batch, bounded functions", m_functions);
        V3Stats::addStatSum("RepCut native batch, domain-specific writes", m_domainWrites);
    }
};
}  // namespace

void batchNativeKernels(AstNetlist* netlistp, const std::vector<V3Sched::LogicByScope*>& logic,
                        V3Order::TrigToSenMap& trigToSen) {
    NativeBatch{netlistp, logic, trigToSen};
}
}  // namespace V3RepCut
