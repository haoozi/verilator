// DESCRIPTION: Verilator: Keyed priority selection runtime oracle
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026-2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

#include "verilated.h"

#include "Vt_repcut_priority.h"

#include <array>
#include <cstdint>
#include <cstdio>
int main() {
    VerilatedContext context;
    context.threads(REPCUT_TEST_THREADS);
    Vt_repcut_priority model{&context};
    std::array<unsigned, 32> expected{}, defaults{}, reverse{};
    uint32_t random = 12345;
    auto next = [&]() {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        return random;
    };
    for (unsigned cycle = 0; cycle < 1000; ++cycle) {
        unsigned ids[8], values[8];
        model.clk = 0;
        model.reset = cycle % 37 == 0;
        model.step = cycle % 5 != 1;
        model.en = cycle % 11 == 0 ? 0 : (cycle % 7 == 0 ? 255 : next());
        for (unsigned i = 0; i < 8; ++i) {
            ids[i] = cycle % 7 == 0 ? cycle % 32 : next() % 32;
            values[i] = next() & 32767;
        }
        model.packet0 = (uint64_t(next()) << 31 ^ next()) & ((uint64_t(1) << 63) - 1);
        model.packet0 = (model.packet0 & ~(uint64_t(31) << 24)) | (uint64_t(ids[0]) << 24);
        model.data0 = values[0];
        model.packet1 = (uint64_t(next()) << 31 ^ next()) & ((uint64_t(1) << 63) - 1);
        model.packet1 = (model.packet1 & ~(uint64_t(31) << 24)) | (uint64_t(ids[1]) << 24);
        model.data1 = values[1];
        model.packet2 = (uint64_t(next()) << 31 ^ next()) & ((uint64_t(1) << 63) - 1);
        model.packet2 = (model.packet2 & ~(uint64_t(31) << 24)) | (uint64_t(ids[2]) << 24);
        model.data2 = values[2];
        model.packet3 = (uint64_t(next()) << 31 ^ next()) & ((uint64_t(1) << 63) - 1);
        model.packet3 = (model.packet3 & ~(uint64_t(31) << 24)) | (uint64_t(ids[3]) << 24);
        model.data3 = values[3];
        model.packet4 = (uint64_t(next()) << 31 ^ next()) & ((uint64_t(1) << 63) - 1);
        model.packet4 = (model.packet4 & ~(uint64_t(31) << 24)) | (uint64_t(ids[4]) << 24);
        model.data4 = values[4];
        model.packet5 = (uint64_t(next()) << 31 ^ next()) & ((uint64_t(1) << 63) - 1);
        model.packet5 = (model.packet5 & ~(uint64_t(31) << 24)) | (uint64_t(ids[5]) << 24);
        model.data5 = values[5];
        model.packet6 = (uint64_t(next()) << 31 ^ next()) & ((uint64_t(1) << 63) - 1);
        model.packet6 = (model.packet6 & ~(uint64_t(31) << 24)) | (uint64_t(ids[6]) << 24);
        model.data6 = values[6];
        model.packet7 = (uint64_t(next()) << 31 ^ next()) & ((uint64_t(1) << 63) - 1);
        model.packet7 = (model.packet7 & ~(uint64_t(31) << 24)) | (uint64_t(ids[7]) << 24);
        model.data7 = values[7];
        for (unsigned i = 0; i < 4; ++i) model.mask[i] = next();
        model.eval();
        model.eval();
        for (unsigned k = 0; k < 32; ++k) {
            if (model.reset)
                expected[k] = defaults[k] = reverse[k] = k + 1;
            else if (model.step) {
                defaults[k] = k ^ 123;
                for (unsigned i = 0; i < 8; ++i)
                    if ((model.en >> i & 1) && ids[i] == k) {
                        expected[k] = defaults[k] = (values[i] + 1) & 32767;
                        break;
                    }
                for (int i = 7; i >= 0; --i)
                    if ((model.en >> i & 1) && ids[i] == k) {
                        reverse[k] = (values[i] + 1) & 32767;
                        break;
                    }
            }
        }
        model.clk = 1;
        model.eval();
        model.eval();
        for (unsigned k = 0; k < 32; ++k) {
            unsigned got = 0, gotDefault = 0, gotReverse = 0;
            for (unsigned b = 0; b < 15; ++b) {
                got |= ((model.q[(15 * k + b) / 32] >> ((15 * k + b) % 32)) & 1) << b;
                gotDefault |= ((model.qd[(15 * k + b) / 32] >> ((15 * k + b) % 32)) & 1) << b;
                gotReverse |= ((model.qr[(15 * k + b) / 32] >> ((15 * k + b) % 32)) & 1) << b;
            }
            if (gotDefault != defaults[k] || gotReverse != reverse[k]) {
                std::fprintf(stderr, "cycle=%u key=%u default=%u/%u reverse=%u/%u\n", cycle, k,
                             gotDefault, defaults[k], gotReverse, reverse[k]);
                return 3;
            }
            if (got != expected[k]) {
                std::fprintf(stderr, "cycle=%u key=%u got=%u expected=%u\n", cycle, k, got,
                             expected[k]);
                return 1;
            }
        }
        for (unsigned k = 0; k < 128; ++k) {
            unsigned want = ((model.packet0 & 127) == k) ^ ((model.mask[k / 32] >> (k % 32)) & 1);
            if (((model.decoded[k / 32] >> (k % 32)) & 1) != want) return 2;
        }
    }
    model.final();
    std::puts("*-* All Finished *-*");
}
