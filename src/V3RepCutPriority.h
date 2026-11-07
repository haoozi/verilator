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

#ifndef VERILATOR_V3REPCUTPRIORITY_H_
#define VERILATOR_V3REPCUTPRIORITY_H_
#include "config_build.h"
#include "verilatedos.h"

#include <set>
#include <string>
#include <vector>
class AstNodeStmt;
class AstScope;
class AstVarScope;
namespace V3RepCut {
void lowerPrioritySelections(AstScope& scope, std::vector<AstNodeStmt*>& statements,
                             const std::set<AstVarScope*>& inputs,
                             const std::set<AstVarScope*>& preserve, const std::string& tag,
                             uint32_t storageGroup) VL_MT_DISABLED;
}
#endif
