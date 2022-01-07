///////////////////////////////////////////
// muldiv.sv
//
// Written: David_Harris@hmc.edu 9 January 2021
// Modified: 
//
// Purpose: M extension multiply and divide
// 
// A component of the Wally configurable RISC-V project.
// 
// Copyright (C) 2021 Harvey Mudd College & Oklahoma State University
//
// MIT LICENSE
// Permission is hereby granted, free of charge, to any person obtaining a copy of this 
// software and associated documentation files (the "Software"), to deal in the Software 
// without restriction, including without limitation the rights to use, copy, modify, merge, 
// publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons 
// to whom the Software is furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in all copies or 
//   substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, 
//   INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR 
//   PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS 
//   BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, 
//   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE 
//   OR OTHER DEALINGS IN THE SOFTWARE.
////////////////////////////////////////////////////////////////////////////////////////////////

`include "wally-config.vh"

module muldiv (
	       input logic 		clk, reset,
	       // Execute Stage interface
	       //    input logic [`XLEN-1:0] 	SrcAE, SrcBE,
		   input logic [`XLEN-1:0] ForwardedSrcAE, ForwardedSrcBE, // *** these are the src outputs before the mux choosing between them and PCE to put in srcA/B
	       input logic [2:0] 	Funct3E, Funct3M,
	       input logic 		MDUE, W64E,
	       // Writeback stage
	       output logic [`XLEN-1:0] MDUResultW,
	       // Divide Done
	       output logic 		DivBusyE, 
	       // hazards
	       input logic 		StallM, StallW, FlushM, FlushW 
	       );

	logic [`XLEN-1:0] MDUResultM;
	logic [`XLEN-1:0] PrelimResultM;
	logic [`XLEN-1:0] QuotM, RemM;
	logic [`XLEN*2-1:0] ProdM; 

	logic 		     DivE;
	logic 		     DivSignedE;	
	logic           W64M; 

	// Multiplier
	mul mul(.clk, .reset, .StallM, .FlushM, .ForwardedSrcAE, .ForwardedSrcBE, .Funct3E, .ProdM);

	// Divide
	// Start a divide when a new division instruction is received and the divider isn't already busy or finishing
	assign DivE = MDUE & Funct3E[2];
	assign DivSignedE = ~Funct3E[0];
	intdivrestoring div(.clk, .reset, .StallM, .DivSignedE, .W64E, .DivE, 
	                    .ForwardedSrcAE, .ForwardedSrcBE, .DivBusyE, .QuotM, .RemM);
		
	// Result multiplexer
	always_comb
		case (Funct3M)	   
			3'b000: PrelimResultM = ProdM[`XLEN-1:0];
			3'b001: PrelimResultM = ProdM[`XLEN*2-1:`XLEN];
			3'b010: PrelimResultM = ProdM[`XLEN*2-1:`XLEN];
			3'b011: PrelimResultM = ProdM[`XLEN*2-1:`XLEN];
			3'b100: PrelimResultM = QuotM;
			3'b101: PrelimResultM = QuotM;
			3'b110: PrelimResultM = RemM;
			3'b111: PrelimResultM = RemM;
		endcase 

	// Handle sign extension for W-type instructions
	flopenrc #(1) W64MReg(clk, reset, FlushM, ~StallM, W64E, W64M);
	if (`XLEN == 64) begin:resmux // RV64 has W-type instructions
		assign MDUResultM = W64M ? {{32{PrelimResultM[31]}}, PrelimResultM[31:0]} : PrelimResultM;
	end else begin:resmux // RV32 has no W-type instructions
		assign MDUResultM = PrelimResultM;
	end

	// Writeback stage pipeline register
	flopenrc #(`XLEN) MDUResultWReg(clk, reset, FlushW, ~StallW, MDUResultM, MDUResultW);	 
endmodule // muldiv

