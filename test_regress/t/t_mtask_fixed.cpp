// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Fixed worker dispatch and object lifetime
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

#include <chrono>
#include <iostream>

static void signalDone(void* vertexp, bool even) {
    static_cast<VlMTaskVertex*>(vertexp)->signalUpstreamDone(even);
}

struct FixedState final {
    VlMTaskVertex done{1};
    VlFixedTask task;
    uint32_t input = 0;
    uint32_t output = 0;
};

static void fixedDone(void* selfp, bool even) {
    FixedState* const statep = static_cast<FixedState*>(selfp);
    statep->output = statep->input ^ (even ? 0x12345678U : 0x98765432U);
    statep->done.signalUpstreamDone(even);
}

static void checkFixed(VlWorkerThread* workerp) {
    for (unsigned lifetime = 0; lifetime < 1000; ++lifetime) {
        // Let the ordinary queue worker park before re-registering fixed work.
        if (lifetime % 100 == 0) std::this_thread::sleep_for(std::chrono::milliseconds{2});
        // Two live models on one worker, with independent alternating epochs.
        // Delete immediately after the final join to exercise descriptor lifetime.
        FixedState* const ap = new FixedState;
        FixedState* const bp = new FixedState;
        ap->task.configure(workerp, fixedDone, ap);
        bp->task.configure(workerp, fixedDone, bp);
        bool epochs[2] = {false, false};
        for (unsigned i = 0; i < 32; ++i) {
            const unsigned owner = (i % 3) == 0;
            FixedState* const statep = owner ? bp : ap;
            const bool even = epochs[owner] = !epochs[owner];
            statep->input = lifetime * 32 + i;
            statep->task.start(even);
            statep->done.waitUntilUpstreamDone(even);
            if (statep->output != (statep->input ^ (even ? 0x12345678U : 0x98765432U))) {
                VL_FATAL_MT(__FILE__, __LINE__, "", "Fixed dispatch lost input or epoch");
            }
            // Setup, ordinary model work and wait markers must still progress
            // while fixed entries are registered on the same pool.
            if (i % 7 == 0) workerp->wait();
        }
        delete ap;
        delete bp;
        workerp->wait();
    }
}

static void checkSkipped(VlWorkerThread* workerp) {
    VlMTaskVertex done{4};
    VlFixedTask task;
    task.configure(workerp, signalDone, &done);
    bool even = false;
    for (unsigned i = 0; i < 10000; ++i) {
        // Alternate partial and full participation. Caller-only invocations do
        // not change the epoch, including consecutive caller-only invocations.
        if (i % 7 < 2) continue;
        even = !even;
        const unsigned skipped = i % 4;
        done.prepareSkipped(even, skipped);
        task.start(even);
        for (unsigned n = skipped + 1; n < 4; ++n) done.signalUpstreamDone(even);
        done.waitUntilUpstreamDone(even);
    }
    workerp->wait();
}

int main() {
    VerilatedContext context;
    context.threads(2);
    VlWorkerThread* const workerp = static_cast<VlThreadPool*>(context.threadPoolp())->workerp(0);
    checkFixed(workerp);
    checkSkipped(workerp);
    std::cout << "*-* All Finished *-*\n";
    return 0;
}
