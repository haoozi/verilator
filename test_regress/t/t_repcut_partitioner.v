// DESCRIPTION: Verilator: Automatic RepCut partitioning with external tools
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module t (
    input logic clk,
    input logic [30:0] data0,
    data1,
    data2,
    data3,
    data4,
    data5,
    data6,
    data7,
    output logic [30:0] result0,
    result1,
    result2,
    result3,
    result4,
    result5,
    result6,
    result7
);
  parameter bit SHARED = 0;
  wire [30:0] common_value = (data0 + data1) ^ (data2 + data3);
  // verilog_format: off
`define LANE(n) \
  always @(posedge clk) \
    result``n <= ((SHARED ? common_value : data``n) << 1) + 31'(n + 1);
  `LANE(0)
  `LANE(1)
  `LANE(2)
  `LANE(3)
  `LANE(4)
  `LANE(5)
  `LANE(6)
  `LANE(7)
`undef LANE
  // verilog_format: on
endmodule
