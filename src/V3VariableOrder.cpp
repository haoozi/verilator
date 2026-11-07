// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Variable ordering
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2003-2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
// V3VariableOrder's Transformations:
//
// Each module:
//   Order module variables
//
//*************************************************************************

#include "V3PchAstMT.h"

#include "V3VariableOrder.h"

#include "V3AstUserAllocator.h"
#include "V3EmitCBase.h"
#include "V3ExecGraph.h"
#include "V3RepCut.h"
#include "V3ThreadPool.h"

#include <algorithm>
#include <map>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

using MTaskIdVec = std::vector<bool>;  // Used as a bit-set indexed by MTask ID
using MTaskAffinityMap = std::unordered_map<const AstVar*, MTaskIdVec>;

struct RepCutReaders final {
    MTaskIdVec readers;
    size_t firstRead = 0;
};
using RepCutReaderMap = std::unordered_map<const AstVar*, RepCutReaders>;
using RepCutWordWriters = std::map<std::pair<const AstVar*, int>, uint32_t>;

// Follow actual worker calls after optimization and chunk construction. Read
// affinity uses physical worker slots, so edge and non-edge jobs share identities.
class GatherRepCutAccess final : VNVisitorConst {
    const VNUser2InUse m_user2InUse;  // AstCFunc::user2: Already visited this job
    RepCutReaderMap& m_readers;
    const std::unordered_set<const AstVar*>& m_fields;
    std::unordered_set<const AstVar*>& m_written;
    RepCutWordWriters& m_wordWriters;
    const AstWordSel* m_wordp = nullptr;
    const uint32_t m_thread;
    size_t& m_order;

    void visit(AstNodeVarRef* nodep) override {
        const AstVar* const varp = nodep->varp();
        if (!m_fields.count(varp)) return;  // Stack temporaries need no model layout.
        // Shared synchronization objects are deliberately written by all workers.
        if (varp->basicp() && varp->basicp()->keyword() == VBasicDTypeKwd::MTASKSTATE) return;
        if (nodep->access().isWriteOrRW()) {
            const uint32_t group = varp->repcutStorageGroup();
            const uint32_t threads = v3Global.opt.threads();
            if (group) {
                UASSERT_OBJ((V3RepCut::storageOwner(group) + threads - 1) % threads == m_thread,
                            varp, "RepCut worker writes another owner's storage");
            } else {
                // Terminal output packing can have independent owners for
                // complete C++ words. A whole-field layout group cannot express
                // this. Audit actual constant word writes across every worker.
                const AstConst* const indexp = m_wordp ? VN_CAST(m_wordp->bitp(), Const) : nullptr;
                UASSERT_OBJ(varp->isPrimaryIO() && varp->direction() == VDirection::OUTPUT
                                && varp->isWide() && indexp && m_wordp->fromp() == nodep,
                            varp,
                            "RepCut worker writes storage without an owner: " << varp->name());
                const int index = indexp->toSInt();
                UASSERT_OBJ(index >= 0 && index < varp->widthWords(), nodep,
                            "RepCut output word is out of bounds");
                const auto owner = m_wordWriters.emplace(std::make_pair(varp, index), m_thread);
                UASSERT_OBJ(owner.first->second == m_thread, nodep,
                            "RepCut workers write the same output word");
            }
            m_written.insert(varp);
        }
        if (nodep->access().isReadOrRW()) {
            auto it = m_readers.find(varp);
            if (it == m_readers.end()) {
                it = m_readers
                         .emplace(varp, RepCutReaders{MTaskIdVec(v3Global.opt.threads(), false),
                                                      m_order})
                         .first;
            }
            ++m_order;
            it->second.readers[m_thread] = true;
        }
    }
    void visit(AstCFunc* nodep) override {
        if (!nodep->user2SetOnce()) iterateChildrenConst(nodep);
    }
    void visit(AstWordSel* nodep) override {
        {
            VL_RESTORER(m_wordp);
            m_wordp = nodep;
            iterateConst(nodep->fromp());
        }
        iterateConst(nodep->bitp());
    }
    void visit(AstNodeCCall* nodep) override {
        iterateChildrenConst(nodep);
        iterateConst(nodep->funcp());
    }
    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

public:
    GatherRepCutAccess(const ExecMTask* taskp, RepCutReaderMap& readers,
                       const std::unordered_set<const AstVar*>& fields,
                       std::unordered_set<const AstVar*>& written, RepCutWordWriters& wordWriters,
                       size_t& order)
        : m_readers{readers}
        , m_fields{fields}
        , m_written{written}
        , m_wordWriters{wordWriters}
        , m_thread{taskp->fixedThread()}
        , m_order{order} {
        UASSERT_OBJ(taskp->hasFixedThread(), taskp->funcp(), "RepCut job lacks a fixed worker");
        iterateConst(taskp->funcp());
    }
};

// Trace through code reachable form an MTask and annotate referenced variabels
class GatherMTaskAffinity final : VNVisitorConst {
    // NODE STATE
    //  AstCFunc::user1()  // bool: Already traced this function
    //  AstVar::user1()  // bool: Already traced this variable
    const VNUser1InUse m_user1InUse;

    // STATE
    MTaskAffinityMap& m_results;  // The result map being built;
    const uint32_t m_id;  // Id of mtask being analysed
    const size_t m_usedIds = ExecMTask::numUsedIds();  // Value of max id + 1

    // CONSTRUCTOR
    GatherMTaskAffinity(const ExecMTask* mTaskp, MTaskAffinityMap& results)
        : m_results{results}
        , m_id{mTaskp->id()} {
        iterateConst(mTaskp->funcp());
    }
    ~GatherMTaskAffinity() = default;
    VL_UNMOVABLE(GatherMTaskAffinity);

    // VISIT
    void visit(AstNodeVarRef* nodep) override {
        // Cheaper than relying on emplace().second
        if (nodep->user1SetOnce()) return;
        AstVar* const varp = nodep->varp();
        // Set affinity bit
        MTaskIdVec& affinity = m_results
                                   .emplace(std::piecewise_construct,  //
                                            std::forward_as_tuple(varp),  //
                                            std::forward_as_tuple(m_usedIds))
                                   .first->second;
        affinity[m_id] = true;
    }

    void visit(AstCFunc* nodep) override {
        if (nodep->user1SetOnce()) return;  // Prevent repeat traversals/recursion
        iterateChildrenConst(nodep);
    }

    void visit(AstNodeCCall* nodep) override {
        iterateChildrenConst(nodep);  // Arguments
        iterateConst(nodep->funcp());  // Callee
    }

    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

public:
    static void apply(const ExecMTask* mTaskp, MTaskAffinityMap& results) {
        GatherMTaskAffinity{mTaskp, results};
    }
};

struct VarAttributes final {
    uint8_t stratum;  // Roughly equivalent to alignment requirement, to avoid padding
    bool anonOk;  // Can be emitted as part of anonymous structure
};
class VariableOrder final {
    std::unordered_map<const AstVar*, VarAttributes> m_attributes;

    const MTaskAffinityMap& m_mTaskAffinity;
    const RepCutReaderMap& m_repcutReaders;
    std::vector<AstVar*>& m_varps;

    VariableOrder(AstNodeModule* modp, const MTaskAffinityMap& mTaskAffinity,
                  const RepCutReaderMap& repcutReaders, std::vector<AstVar*>& varps)
        : m_mTaskAffinity{mTaskAffinity}
        , m_repcutReaders{repcutReaders}
        , m_varps{varps} {
        orderModuleVars(modp);
    }
    ~VariableOrder() = default;
    VL_UNCOPYABLE(VariableOrder);

    //######################################################################

    // Simple sort
    void simpleSortVars(std::vector<AstVar*>& varps) {
        stable_sort(varps.begin(), varps.end(),
                    [this](const AstVar* ap, const AstVar* bp) -> bool {
                        if (ap->isStatic() != bp->isStatic()) {  // Non-statics before statics
                            return bp->isStatic();
                        }
                        UASSERT(m_attributes.find(ap) != m_attributes.end()
                                    && m_attributes.find(bp) != m_attributes.end(),
                                "m_attributes should be populated for each AstVar");
                        const auto& attrA = m_attributes.at(ap);
                        const auto& attrB = m_attributes.at(bp);
                        if (attrA.anonOk != attrB.anonOk) {  // Anons before non-anons
                            return attrA.anonOk;
                        }
                        return attrA.stratum < attrB.stratum;  // Finally sort by stratum
                    });
    }

    static bool emptyAffinity(const MTaskIdVec& vec) {
        return std::find(vec.begin(), vec.end(), true) == vec.end();
    }

    // Explicit writer ownership takes precedence over read affinity. Candidate
    // and state roles have separate boundaries, including the trailing ordinary
    // variables. Early state writes cannot invalidate neighboring shared-state
    // fields while another worker is still reading them during next evaluation.
    void repcutSortVars(std::vector<AstVar*>& varps) {
        std::map<uint32_t, std::vector<AstVar*>> groups;
        for (AstVar* const varp : varps) groups[varp->repcutStorageGroup()].push_back(varp);
        varps.clear();
        const auto append = [this, &varps](std::vector<AstVar*>& group, bool& aligned) {
            // First-use order reflects optimized sequential evaluation. Keep that
            // order within equal alignment strata rather than adding padding
            // between every differently sized field.
            std::stable_sort(group.begin(), group.end(),
                             [this](const AstVar* ap, const AstVar* bp) {
                                 const auto a = m_repcutReaders.find(ap);
                                 const auto b = m_repcutReaders.find(bp);
                                 if (a == m_repcutReaders.end()) return false;
                                 if (b == m_repcutReaders.end()) return true;
                                 return a->second.firstRead < b->second.firstRead;
                             });
            simpleSortVars(group);
            for (AstVar* const varp : group) {
                if (!aligned && !varp->isStatic()) {
                    varp->mtaskCacheLineAlign(true);
                    aligned = true;
                }
                varps.push_back(varp);
            }
        };
        size_t readerGroups = 0;
        const MTaskIdVec noReaders(v3Global.opt.threads(), false);
        for (const auto& pair : groups) {
            if (!pair.first) continue;
            std::map<MTaskIdVec, std::vector<AstVar*>> byReaders;
            for (AstVar* const varp : pair.second) {
                const auto it = m_repcutReaders.find(varp);
                byReaders[it == m_repcutReaders.end() ? noReaders : it->second.readers].push_back(
                    varp);
            }
            bool aligned = false;
            for (auto& readers : byReaders) append(readers.second, aligned);
            readerGroups += byReaders.size();
        }
        // Ordinary fields retain their previous order and a separate boundary.
        simpleSortVars(groups[0]);
        bool aligned = false;
        for (AstVar* const varp : groups[0]) {
            if (!aligned && !varp->isStatic()) {
                varp->mtaskCacheLineAlign(true);
                aligned = true;
            }
            varps.push_back(varp);
        }
        V3Stats::addStatSum("VariableOrder, RepCut reader groups", readerGroups);
        V3Stats::addStatSum("VariableOrder, RepCut storage groups", groups.size() - 1);
    }

    // Sort by MTask-affinity first, then the same as simpleSortVars
    void mtaskSortVars(std::vector<AstVar*>& varps) {
        // Map from "MTask affinity" -> "variable list"
        std::map<MTaskIdVec, std::vector<AstVar*>> m2v;
        const MTaskIdVec emptyVec(ExecMTask::numUsedIds(), false);
        for (AstVar* const varp : varps) {
            const auto it = m_mTaskAffinity.find(varp);
            const MTaskIdVec& key = it == m_mTaskAffinity.end() ? emptyVec : it->second;
            m2v[key].push_back(varp);
        }

        varps.clear();

        // Helper function to sort given vector, then append to 'varps'
        const auto sortAndAppend
            = [this, &varps](std::vector<AstVar*>& subVarps, bool alignFirst) {
                  simpleSortVars(subVarps);
                  bool aligned = !alignFirst;
                  for (AstVar* const varp : subVarps) {
                      if (!aligned && !varp->isStatic()) {
                          varp->mtaskCacheLineAlign(true);
                          V3Stats::addStatSum("VariableOrder, MTask aligned group starts", 1);
                          aligned = true;
                      }
                      varps.push_back(varp);
                  }
              };

        // Sort non-empty MTask affinity groups in the map's deterministic key order. This keeps
        // memory linear in the number of affinity groups, unlike the old complete
        // pairwise-distance ordering.
        size_t affinityGroups = 0;
        for (auto& pair : m2v) {
            if (emptyAffinity(pair.first)) continue;
            sortAndAppend(pair.second, true);
            ++affinityGroups;
        }

        // Finally add the variables with no known MTask affinity
        sortAndAppend(m2v[emptyVec], false);

        V3Stats::addStatSum("VariableOrder, MTask affinity groups", affinityGroups);
        V3Stats::addStatSum("VariableOrder, no-affinity variables", m2v[emptyVec].size());
    }

    // cppcheck-suppress constParameterPointer
    void orderModuleVars(AstNodeModule* modp) {
        // Unlink all module variables from the module, compute attributes
        for (AstNode *nodep = modp->stmtsp(), *nextp; nodep; nodep = nextp) {
            nextp = nodep->nextp();
            if (AstVar* const varp = VN_CAST(nodep, Var)) {
                m_varps.push_back(varp);

                // Compute attributes up front
                // Stratum
                const int sigbytes = varp->dtypeSkipRefp()->widthAlignBytes();
                const uint8_t stratum = (v3Global.opt.hierChild() && varp->isPrimaryIO())   ? 0
                                        : (varp->isPrimaryClock() && varp->widthMin() == 1) ? 1
                                        : VN_IS(varp->dtypeSkipRefp(), UnpackArrayDType)    ? 9
                                        : (varp->basicp() && varp->basicp()->isOpaque())    ? 8
                                        : (varp->isScBv() || varp->isScBigUint())           ? 7
                                        : (sigbytes == 8)                                   ? 6
                                        : (sigbytes == 4)                                   ? 5
                                        : (sigbytes == 2)                                   ? 3
                                        : (sigbytes == 1)                                   ? 2
                                                                                            : 10;
                m_attributes.emplace(varp, VarAttributes{stratum, EmitCUtil::isAnonOk(varp)});
            }
        }

        if (!m_varps.empty()) {
            if (std::any_of(m_varps.begin(), m_varps.end(),
                            [](const AstVar* varp) { return varp->repcutStorageGroup() != 0; })) {
                repcutSortVars(m_varps);
            } else if (!v3Global.opt.mtasks()) {
                simpleSortVars(m_varps);
            } else {
                mtaskSortVars(m_varps);
            }
        }
    }

public:
    static void processModule(AstNodeModule* modp, const MTaskAffinityMap& mTaskAffinity,
                              const RepCutReaderMap& repcutReaders,
                              std::vector<AstVar*>& varps) VL_MT_STABLE {
        VariableOrder{modp, mTaskAffinity, repcutReaders, varps};
    }
};

//######################################################################
// V3VariableOrder static functions

void V3VariableOrder::orderAll(AstNetlist* netlistp) {
    UINFO(2, __FUNCTION__ << ":");

    MTaskAffinityMap mTaskAffinity;
    RepCutReaderMap repcutReaders;
    std::unordered_set<const AstVar*> repcutWritten;
    RepCutWordWriters repcutWordWriters;
    std::unordered_set<const AstVar*> modelFields;
    for (const AstNodeModule* modp = netlistp->modulesp(); modp;
         modp = VN_AS(modp->nextp(), NodeModule)) {
        for (AstNode* nodep = modp->stmtsp(); nodep; nodep = nodep->nextp()) {
            if (const AstVar* const varp = VN_CAST(nodep, Var)) modelFields.insert(varp);
        }
    }

    // Gather MTask affinities
    if (v3Global.opt.mtasks()) {
        size_t repcutReadOrder = 0;
        netlistp->topModulep()->foreach([&](AstExecGraph* execGraphp) {
            for (const V3GraphVertex& vtx : execGraphp->depGraphp()->vertices()) {
                const ExecMTask* const taskp = vtx.as<const ExecMTask>();
                GatherMTaskAffinity::apply(taskp, mTaskAffinity);
                if (taskp->funcp()->isRepCut())
                    GatherRepCutAccess{taskp,         repcutReaders,     modelFields,
                                       repcutWritten, repcutWordWriters, repcutReadOrder};
            }
        });
    }
    if (v3Global.opt.stats()) V3Stats::statsStage("variableorder-gather");
    if (!repcutWritten.empty())
        V3Stats::addStat("VariableOrder, RepCut audited worker-written fields",
                         repcutWritten.size());
    if (!repcutWordWriters.empty())
        V3Stats::addStat("VariableOrder, RepCut audited worker-written output words",
                         repcutWordWriters.size());

    // Sort variables for each module
    std::unordered_map<AstNodeModule*, std::vector<AstVar*>> sortedVars;
    {
        V3ThreadScope threadScope;

        for (AstNodeModule* modp = v3Global.rootp()->modulesp(); modp;
             modp = VN_AS(modp->nextp(), NodeModule)) {
            std::vector<AstVar*>& varps = sortedVars[modp];
            threadScope.enqueue([modp, &mTaskAffinity, &repcutReaders, &varps]() {
                VariableOrder::processModule(modp, mTaskAffinity, repcutReaders, varps);
            });
        }
    }
    if (v3Global.opt.stats()) V3Stats::statsStage("variableorder-sort");

    // Insert them back under the module, in the new order, but at
    // the front of the list so they come out first in dumps/JSON.
    for (AstNodeModule* modp = v3Global.rootp()->modulesp(); modp;
         modp = VN_AS(modp->nextp(), NodeModule)) {
        const std::vector<AstVar*>& varps = sortedVars[modp];

        if (!varps.empty()) {
            auto it = varps.cbegin();
            AstVar* const firstp = *it++;
            firstp->unlinkFrBack();
            for (; it != varps.cend(); ++it) {
                AstVar* const varp = *it;
                varp->unlinkFrBack();
                firstp->addNext(varp);
            }
            if (AstNode* const stmtsp = modp->stmtsp()) {
                stmtsp->unlinkFrBackWithNext();
                AstNode::addNext<AstNode, AstNode>(firstp, stmtsp);
            }
            modp->addStmtsp(firstp);
        }
    }

    // Done
    V3Global::dumpCheckGlobalTree("variableorder", 0, dumpTreeEitherLevel() >= 3);
}
