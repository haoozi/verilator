// DESCRIPTION: Verilator: Automatic partitioner reference model
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

#include "verilated.h"

#include "Vt_repcut_partitioner.h"

// These require the above. Comment prevents clang-format moving them
#include "TestCheck.h"

int errors = 0;

int main() {
    VerilatedContext context;
    context.threads(2);
    context.randReset(0);
    Vt_repcut_partitioner model{&context};
    IData* const inputs[] = {&model.data0, &model.data1, &model.data2, &model.data3,
                             &model.data4, &model.data5, &model.data6, &model.data7};
    const IData* const outputs[]
        = {&model.result0, &model.result1, &model.result2, &model.result3,
           &model.result4, &model.result5, &model.result6, &model.result7};
    IData expected[8] = {};
    IData random = 17;
    for (unsigned cycle = 0; cycle < 4096; ++cycle) {
        model.clk = 0;
        for (unsigned i = 0; i < 8; ++i) {
            random = random * 1664525U + 1013904223U;
            *inputs[i] = random & 0x7fffffffU;
        }
        model.eval();
        const IData commonValue = (*inputs[0] + *inputs[1]) ^ (*inputs[2] + *inputs[3]);
        model.clk = 1;
        model.eval();
        for (unsigned i = 0; i < 8; ++i) {
            const IData input = REPCUT_SHARED ? commonValue : *inputs[i];
            expected[i] = ((input << 1) + i + 1) & 0x7fffffffU;
            TEST_CHECK_EQ(*outputs[i], expected[i]);
        }
        model.eval();
        for (unsigned i = 0; i < 8; ++i) TEST_CHECK_EQ(*outputs[i], expected[i]);
    }
    model.final();
    if (errors) return 1;
    VL_PRINTF("*-* All Finished *-*\n");
    return 0;
}
