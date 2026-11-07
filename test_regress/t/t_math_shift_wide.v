// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Verilator contributors
// SPDX-License-Identifier: CC0-1.0

// verilog_format: off
`define stop $stop
`define checkh(gotv, expv) do if ((gotv) !== (expv)) begin $write("%%Error: %s:%0d:  got=%p exp=%p\n", `__FILE__, `__LINE__, (gotv), (expv)); `stop; end while(0);
// verilog_format: on

module t(
    input clk
);
  wire [5:0] done;
  shift_check #(65) c0(clk, done[0]);
  shift_check #(95) c1(clk, done[1]);
  shift_check #(129) c2(clk, done[2]);
  shift_check #(256) c3(clk, done[3]);
  shift_check #(2063) c4(clk, done[4]);
  shift_check #(4096) c5(clk, done[5]);

  always @(posedge clk) begin
    if (&done) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end
endmodule

module shift_check #(parameter WIDTH = 95)(input clk, output bit done = 0);
  bit [63:0] state = 64'hf87d21ac901eb563;
  int unsigned iteration = 0;
  int unsigned amount;
  bit [63:0] quad_amount;
  bit [95:0] wide_amount;
  bit [WIDTH-1:0] data;
  bit [WIDTH-1:0] expected;
  bit [WIDTH-1:0] self_value;
  bit [WIDTH-1:0] array_value [2];
  bit slot;

  always @(posedge clk) begin
    if (!done) begin
      state = {state[62:0], state[63] ^ state[62] ^ state[60] ^ state[59]};
      for (int i = 0; i < WIDTH; ++i) data[i] = state[i % 64] ^ 1'(i + iteration);
      if (iteration < 70) amount = iteration;
      else if (iteration < 140) amount = WIDTH - 1 - ((iteration - 70) % WIDTH);
      else if (iteration < 170) amount = WIDTH + iteration;
      else amount = state[31:0] % (WIDTH + 33);

      // Independent bit selection oracle, without another shift expression.
      expected = '0;
      for (int i = 0; i < WIDTH; ++i) begin
        if (amount < WIDTH && i >= amount) expected[i] = data[i - amount];
      end
      `checkh(data << amount, expected);
      `checkh($signed(data) <<< amount, expected);
      self_value = data;
      self_value <<= amount;
      `checkh(self_value, expected);
      slot = iteration[0];
      array_value[slot] = data;
      array_value[slot] = array_value[slot] << amount;
      `checkh(array_value[slot], expected);

      quad_amount = {32'b0, amount};
      wide_amount = {64'b0, amount};
      if (iteration[3:0] == 15) begin
        quad_amount[40] = 1;
        wide_amount[80] = 1;
        expected = '0;
      end
      `checkh(data << quad_amount, expected);
      `checkh(data << wide_amount, expected);
      if (++iteration == 200) done = 1;
    end
  end
endmodule
