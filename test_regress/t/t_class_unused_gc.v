// DESCRIPTION: Verilator: Emit garbage collection only for surviving classes
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

// verilog_format: off
`define stop $stop
`define checkd(gotv,expv) do if ((gotv) !== (expv)) begin $write("%%Error: %s:%0d: got=%0d exp=%0d (%s !== %s)\n", `__FILE__,`__LINE__, (gotv), (expv), `"gotv`", `"expv`"); `stop; end while(0);
// verilog_format: on

class Item #(parameter int WIDTH = 32);
  bit [WIDTH-1:0] value;
  function new(int v);
    value = v;
  endfunction
endclass

module t (clk);
  input clk;
  int count = 0;
`ifdef USE_CLASS
  Item#(32) item;
`endif
  always @(posedge clk) begin
    count = count + 1;
`ifdef USE_CLASS
    item = new(count);
    `checkd(item.value, count);
    item = null;
`endif
    if (count == 40) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end
endmodule
