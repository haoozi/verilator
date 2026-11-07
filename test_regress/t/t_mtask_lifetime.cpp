// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: MTask completion and object lifetime
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

#include "verilated.h"
#include "verilated_threads.h"

#include <iostream>

static void signalDone(void* vertexp, bool even) {
    static_cast<VlMTaskVertex*>(vertexp)->signalUpstreamDone(even);
}

int main() {
    VerilatedContext context;
    context.threads(2);
    VlWorkerThread* const workerp = static_cast<VlThreadPool*>(context.threadPoolp())->workerp(0);
    for (unsigned i = 0; i < 10000; ++i) {
        const bool even = (i & 1) == 0;
        VlMTaskVertex* const vertexp = new VlMTaskVertex{1};
        if (!even) vertexp->signalUpstreamDone(true);
        workerp->addTask(signalDone, vertexp, even);
        vertexp->waitUntilUpstreamDone(even);
        // The completion notification must be the worker's last vertex access.
        delete vertexp;
    }
    workerp->wait();
    std::cout << "*-* All Finished *-*\n";
    return 0;
}
