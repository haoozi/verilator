// DESCRIPTION: Verilator: Keyed priority selection semantics
// SPDX-FileCopyrightText: 2026-2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0
module t (
  input clk,
  input reset,
  input step,
  input [7:0] en,
  input [62:0] packet0,
  input [62:0] packet1,
  input [62:0] packet2,
  input [62:0] packet3,
  input [62:0] packet4,
  input [62:0] packet5,
  input [62:0] packet6,
  input [62:0] packet7,
  input [14:0] data0,
  input [14:0] data1,
  input [14:0] data2,
  input [14:0] data3,
  input [14:0] data4,
  input [14:0] data5,
  input [14:0] data6,
  input [14:0] data7,
  input [127:0] mask,
  output wire [479:0] q,
  output wire [479:0] qd,
  output wire [479:0] qr,
  output reg [30:0] witness,
  output wire [127:0] decoded
);
  always @(posedge clk) witness <= (31'(data0) / (31'(packet0) | 31'd1))
      ^ (31'(data1) / (31'(packet1) | 31'd3))
      ^ (31'(data2) / (31'(packet2) | 31'd5))
      ^ (31'(data3) / (31'(packet3) | 31'd7))
      ^ (31'(data4) / (31'(packet4) | 31'd9))
      ^ (31'(data5) / (31'(packet5) | 31'd11))
      ^ (31'(data6) / (31'(packet6) | 31'd13))
      ^ (31'(data7) / (31'(packet7) | 31'd15))
      ^ (31'(data0) / (31'(packet0) | 31'd17))
      ^ (31'(data1) / (31'(packet1) | 31'd19))
      ^ (31'(data2) / (31'(packet2) | 31'd21))
      ^ (31'(data3) / (31'(packet3) | 31'd23))
      ^ (31'(data4) / (31'(packet4) | 31'd25))
      ^ (31'(data5) / (31'(packet5) | 31'd27))
      ^ (31'(data6) / (31'(packet6) | 31'd29))
      ^ (31'(data7) / (31'(packet7) | 31'd31))
      ^ (31'(data0) / (31'(packet0) | 31'd33))
      ^ (31'(data1) / (31'(packet1) | 31'd35))
      ^ (31'(data2) / (31'(packet2) | 31'd37))
      ^ (31'(data3) / (31'(packet3) | 31'd39))
      ^ (31'(data4) / (31'(packet4) | 31'd41))
      ^ (31'(data5) / (31'(packet5) | 31'd43))
      ^ (31'(data6) / (31'(packet6) | 31'd45))
      ^ (31'(data7) / (31'(packet7) | 31'd47))
      ^ (31'(data0) / (31'(packet0) | 31'd49))
      ^ (31'(data1) / (31'(packet1) | 31'd51))
      ^ (31'(data2) / (31'(packet2) | 31'd53))
      ^ (31'(data3) / (31'(packet3) | 31'd55))
      ^ (31'(data4) / (31'(packet4) | 31'd57))
      ^ (31'(data5) / (31'(packet5) | 31'd59))
      ^ (31'(data6) / (31'(packet6) | 31'd61))
      ^ (31'(data7) / (31'(packet7) | 31'd63))
      ^ (31'(data0) / (31'(packet0) | 31'd65))
      ^ (31'(data1) / (31'(packet1) | 31'd67))
      ^ (31'(data2) / (31'(packet2) | 31'd69))
      ^ (31'(data3) / (31'(packet3) | 31'd71))
      ^ (31'(data4) / (31'(packet4) | 31'd73))
      ^ (31'(data5) / (31'(packet5) | 31'd75))
      ^ (31'(data6) / (31'(packet6) | 31'd77))
      ^ (31'(data7) / (31'(packet7) | 31'd79))
      ^ (31'(data0) / (31'(packet0) | 31'd81))
      ^ (31'(data1) / (31'(packet1) | 31'd83))
      ^ (31'(data2) / (31'(packet2) | 31'd85))
      ^ (31'(data3) / (31'(packet3) | 31'd87))
      ^ (31'(data4) / (31'(packet4) | 31'd89))
      ^ (31'(data5) / (31'(packet5) | 31'd91))
      ^ (31'(data6) / (31'(packet6) | 31'd93))
      ^ (31'(data7) / (31'(packet7) | 31'd95))
      ^ (31'(data0) / (31'(packet0) | 31'd97))
      ^ (31'(data1) / (31'(packet1) | 31'd99))
      ^ (31'(data2) / (31'(packet2) | 31'd101))
      ^ (31'(data3) / (31'(packet3) | 31'd103))
      ^ (31'(data4) / (31'(packet4) | 31'd105))
      ^ (31'(data5) / (31'(packet5) | 31'd107))
      ^ (31'(data6) / (31'(packet6) | 31'd109))
      ^ (31'(data7) / (31'(packet7) | 31'd111))
      ^ (31'(data0) / (31'(packet0) | 31'd113))
      ^ (31'(data1) / (31'(packet1) | 31'd115))
      ^ (31'(data2) / (31'(packet2) | 31'd117))
      ^ (31'(data3) / (31'(packet3) | 31'd119))
      ^ (31'(data4) / (31'(packet4) | 31'd121))
      ^ (31'(data5) / (31'(packet5) | 31'd123))
      ^ (31'(data6) / (31'(packet6) | 31'd125))
      ^ (31'(data7) / (31'(packet7) | 31'd127))
      ^ (31'(data0) / (31'(packet0) | 31'd129))
      ^ (31'(data1) / (31'(packet1) | 31'd131))
      ^ (31'(data2) / (31'(packet2) | 31'd133))
      ^ (31'(data3) / (31'(packet3) | 31'd135))
      ^ (31'(data4) / (31'(packet4) | 31'd137))
      ^ (31'(data5) / (31'(packet5) | 31'd139))
      ^ (31'(data6) / (31'(packet6) | 31'd141))
      ^ (31'(data7) / (31'(packet7) | 31'd143))
      ^ (31'(data0) / (31'(packet0) | 31'd145))
      ^ (31'(data1) / (31'(packet1) | 31'd147))
      ^ (31'(data2) / (31'(packet2) | 31'd149))
      ^ (31'(data3) / (31'(packet3) | 31'd151))
      ^ (31'(data4) / (31'(packet4) | 31'd153))
      ^ (31'(data5) / (31'(packet5) | 31'd155))
      ^ (31'(data6) / (31'(packet6) | 31'd157))
      ^ (31'(data7) / (31'(packet7) | 31'd159))
      ^ (31'(data0) / (31'(packet0) | 31'd161))
      ^ (31'(data1) / (31'(packet1) | 31'd163))
      ^ (31'(data2) / (31'(packet2) | 31'd165))
      ^ (31'(data3) / (31'(packet3) | 31'd167))
      ^ (31'(data4) / (31'(packet4) | 31'd169))
      ^ (31'(data5) / (31'(packet5) | 31'd171))
      ^ (31'(data6) / (31'(packet6) | 31'd173))
      ^ (31'(data7) / (31'(packet7) | 31'd175))
      ^ (31'(data0) / (31'(packet0) | 31'd177))
      ^ (31'(data1) / (31'(packet1) | 31'd179))
      ^ (31'(data2) / (31'(packet2) | 31'd181))
      ^ (31'(data3) / (31'(packet3) | 31'd183))
      ^ (31'(data4) / (31'(packet4) | 31'd185))
      ^ (31'(data5) / (31'(packet5) | 31'd187))
      ^ (31'(data6) / (31'(packet6) | 31'd189))
      ^ (31'(data7) / (31'(packet7) | 31'd191))
      ^ (31'(data0) / (31'(packet0) | 31'd193))
      ^ (31'(data1) / (31'(packet1) | 31'd195))
      ^ (31'(data2) / (31'(packet2) | 31'd197))
      ^ (31'(data3) / (31'(packet3) | 31'd199))
      ^ (31'(data4) / (31'(packet4) | 31'd201))
      ^ (31'(data5) / (31'(packet5) | 31'd203))
      ^ (31'(data6) / (31'(packet6) | 31'd205))
      ^ (31'(data7) / (31'(packet7) | 31'd207))
      ^ (31'(data0) / (31'(packet0) | 31'd209))
      ^ (31'(data1) / (31'(packet1) | 31'd211))
      ^ (31'(data2) / (31'(packet2) | 31'd213))
      ^ (31'(data3) / (31'(packet3) | 31'd215))
      ^ (31'(data4) / (31'(packet4) | 31'd217))
      ^ (31'(data5) / (31'(packet5) | 31'd219))
      ^ (31'(data6) / (31'(packet6) | 31'd221))
      ^ (31'(data7) / (31'(packet7) | 31'd223))
      ^ (31'(data0) / (31'(packet0) | 31'd225))
      ^ (31'(data1) / (31'(packet1) | 31'd227))
      ^ (31'(data2) / (31'(packet2) | 31'd229))
      ^ (31'(data3) / (31'(packet3) | 31'd231))
      ^ (31'(data4) / (31'(packet4) | 31'd233))
      ^ (31'(data5) / (31'(packet5) | 31'd235))
      ^ (31'(data6) / (31'(packet6) | 31'd237))
      ^ (31'(data7) / (31'(packet7) | 31'd239))
      ^ (31'(data0) / (31'(packet0) | 31'd241))
      ^ (31'(data1) / (31'(packet1) | 31'd243))
      ^ (31'(data2) / (31'(packet2) | 31'd245))
      ^ (31'(data3) / (31'(packet3) | 31'd247))
      ^ (31'(data4) / (31'(packet4) | 31'd249))
      ^ (31'(data5) / (31'(packet5) | 31'd251))
      ^ (31'(data6) / (31'(packet6) | 31'd253))
      ^ (31'(data7) / (31'(packet7) | 31'd255));
  for (genvar k = 0; k < 32; ++k) begin : g
    reg [14:0] state;


    assign q[15*k +: 15] = state;
    always @(posedge clk) begin
      if (reset) state <= 15'(k+1);
      else if (step) begin
        if (en[0] && packet0[28:24] == 5'(k)) state <= data0 + 15'd1;
        else if (en[1] && packet1[28:24] == 5'(k)) state <= data1 + 15'd1;
        else if (en[2] && packet2[28:24] == 5'(k)) state <= data2 + 15'd1;
        else if (en[3] && packet3[28:24] == 5'(k)) state <= data3 + 15'd1;
        else if (en[4] && packet4[28:24] == 5'(k)) state <= data4 + 15'd1;
        else if (en[5] && packet5[28:24] == 5'(k)) state <= data5 + 15'd1;
        else if (en[6] && packet6[28:24] == 5'(k)) state <= data6 + 15'd1;
        else if (en[7] && packet7[28:24] == 5'(k)) state <= data7 + 15'd1;
      end
    end
  end
  for (genvar k = 0; k < 32; ++k) begin : gd
    reg [14:0] state;


    assign qd[15*k +: 15] = state;
    always @(posedge clk) begin
      if (reset) state <= 15'(k+1);
      else if (step) begin
        if (en[0] && packet0[28:24] == 5'(k)) state <= data0 + 15'd1;
        else if (en[1] && packet1[28:24] == 5'(k)) state <= data1 + 15'd1;
        else if (en[2] && packet2[28:24] == 5'(k)) state <= data2 + 15'd1;
        else if (en[3] && packet3[28:24] == 5'(k)) state <= data3 + 15'd1;
        else if (en[4] && packet4[28:24] == 5'(k)) state <= data4 + 15'd1;
        else if (en[5] && packet5[28:24] == 5'(k)) state <= data5 + 15'd1;
        else if (en[6] && packet6[28:24] == 5'(k)) state <= data6 + 15'd1;
        else if (en[7] && packet7[28:24] == 5'(k)) state <= data7 + 15'd1;
        else state <= 15'(k ^ 123);
      end
    end
  end
  for (genvar k = 0; k < 32; ++k) begin : gr
    reg [14:0] state;


    assign qr[15*k +: 15] = state;
    always @(posedge clk) begin
      if (reset) state <= 15'(k+1);
      else if (step) begin
        if (en[7] && packet7[28:24] == 5'(k)) state <= data7 + 15'd1;
        else if (en[6] && packet6[28:24] == 5'(k)) state <= data6 + 15'd1;
        else if (en[5] && packet5[28:24] == 5'(k)) state <= data5 + 15'd1;
        else if (en[4] && packet4[28:24] == 5'(k)) state <= data4 + 15'd1;
        else if (en[3] && packet3[28:24] == 5'(k)) state <= data3 + 15'd1;
        else if (en[2] && packet2[28:24] == 5'(k)) state <= data2 + 15'd1;
        else if (en[1] && packet1[28:24] == 5'(k)) state <= data1 + 15'd1;
        else if (en[0] && packet0[28:24] == 5'(k)) state <= data0 + 15'd1;
      end
    end
  end
  for (genvar k = 0; k < 128; ++k) begin
    assign decoded[k] = (packet0[6:0] == 7'(k)) ^ mask[k];
  end
endmodule
