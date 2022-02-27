///////////////////////////////////////////
//
// WALLY-TEST-LIB-64.S
//
// Author: Kip Macsai-Goren <kmacsaigoren@g.hmc.edu>
//
// Created 2021-07-19
//
// Copyright (C) 2021 Harvey Mudd College & Oklahoma State University
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,
// modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software
// is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
// BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
// OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
///////////////////////////////////////////

#include "model_test.h"
#include "arch_test.h"

.macro INIT_TESTS

RVTEST_ISA("RV64I")

.section .text.init
.globl rvtest_entry_point
rvtest_entry_point:
RVMODEL_BOOT
RVTEST_CODE_BEGIN



    // ---------------------------------------------------------------------------------------------
    // Initialization Overview:
    //   
    //   Initialize x6 as a virtual pointer to the test results
    //   Initialize x16 as a physical pointer to the test results
    //   Set up stack pointer (sp = x2)
    //   
	// ---------------------------------------------------------------------------------------------

    // address for test results
    la x6, test_1_res
    la x16, test_1_res // x16 reserved for the physical address equivalent of x6 to be used in trap handlers
                        // any time either is used, both must be updated.

    // address for stack
    la sp, top_of_stack

.endm

.macro TRAP_HANDLER MODE, VECTORED=1 // default to vectored tests
    //   Set up the exception Handler, keeping the original handler in x4.

    // trap handler setup
    la x1, trap_handler_\MODE\()
.if (\VECTORED == 1)
    ori x1, x1, 0x1 // set mode field of tvec to 1, forcing vectored interrupts
.endif

.if (\MODE\() == m)
    csrrw x4, \MODE\()tvec, x1  // x4 reserved for "default" trap handler address that needs to be restored before halting this test.
.else
    csrw \MODE\()tvec, x1 // we only neet save the machine trap handler and this if statement ensures it isn't overwritten
.endif

    li a0, 0
    li a1, 0 
    li a2, 0 // reset trap handler inputs to zero

    j trap_handler_end_\MODE\() // skip the trap handler when it is being defined.

	// ---------------------------------------------------------------------------------------------
    // General traps Handler
    // 
    //   Handles traps by branching to different behaviors based on mcause.
    //   
    //   Note that allowing the exception handler to change mode for a program is a huge security
    //   hole, but this is an expedient way of writing tests that need different modes
    // 
    // input parameters:
    // 
    //   a0 (x10): 
    //       0: halt program with no failures
    //       1: halt program with failure in x11 = a1
    //       2: go to machine mode
    //       3: go to supervisor mode
    //       4: go to user mode
    //       others: do nothing
    //
    //   a1 (x11): 
    //       VPN for return address after changing privilege mode.
    //       This should be the base VPN with no offset.
    //       0x0 : defaults to next instruction on the same page the trap was called on.
    //
    //   a2 (x12): 
    //       Pagetype of the current address VPN before changing privilge mode
    //       Used so that we can know how many bits of the adress are the offset.
    //       Ignored if a1 == 0x0
    //       0: Kilopage
    //       1: Megapage
    //       2: Gigapage
    //       3: Terapage
    //     
    // --------------------------------------------------------------------------------------------

.align 2
trap_handler_\MODE\():
    j trap_unvectored_\MODE\() // for the unvectored implimentation: jump past this table of addresses into the actual handler
    // *** ASSUMES that a cause value of 0 for an interrupt is unimplemented
    // otherwise, a vectored interrupt handler should jump to trap_handler_\MODE\() + 4 * Interrupt cause code
    .4byte s_soft_interrupt_\MODE\()    // 1: instruction access fault // the zero spot is taken up by the instruction to skip this table.
    .4byte segfault_\MODE\()            // 2: reserved
    .4byte m_soft_interrupt_\MODE\()    // 3: breakpoint
    .4byte segfault_\MODE\()            // 4: reserved
    .4byte s_time_interrupt_\MODE\()    // 5: load access fault
    .4byte segfault_\MODE\()            // 6: reserved
    .4byte m_time_interrupt_\MODE\()    // 7: store access fault
    .4byte segfault_\MODE\()            // 8: reserved
    .4byte s_ext_interrupt_\MODE\()     // 9: ecall from S-mode
    .4byte segfault_\MODE\()            // 10: reserved
    .4byte m_ext_interrupt_\MODE\()     // 11: ecall from M-mode
    // 12 through >=16 are reserved or designated for platform use

trap_unvectored_\MODE\():
    // save registers on stack before using
    sd x1, -8(sp)       
    sd x5, -16(sp)      

    // Record trap
    csrr x1, \MODE\()cause     // record the mcause
    sd x1, 0(x16)        
    addi x6, x6, 8     
    addi x16, x16, 8    // update pointers for logging results

    // Respond to trap based on cause
    // All interrupts should return after being logged
    li x5, 0x8000000000000000   // if msb is set, it is an interrupt
    and x5, x5, x1
    bnez x5, trapreturn_\MODE\()   // return from interrupt
    // Other trap handling is specified in the vector Table
    slli x1, x1, 3      // multiply cause by 8 to get offset in vector Table
    la x5, exception_vector_table_\MODE\()
    add x5, x5, x1      // compute address of vector in Table
    ld x5, 0(x5)        // fectch address of handler from vector Table
    jr x5               // and jump to the handler
    
segfault_\MODE\():
    ld x5, -16(sp)      // restore registers from stack before faulting
    ld x1, -8(sp)       
    j terminate_test          // halt program.

trapreturn_\MODE\():
    // look at the instruction to figure out whether to add 2 or 4 bytes to PC, or go to address specified in a1
    csrr x1, \MODE\()epc       // get the mepc
    addi x1, x1, 4 // *** should be 2 for compressed instructions, see note.


// ****** KMG: the following is no longer as easy to determine. mepc gets the virtual address of the trapped instruction, 
// ********     but in the handler, we work in M mode with physical addresses
//              This means the address in mepc is suddenly pointing somewhere else.
//              to get this to work, We could either retranslate the vaddr back into a paddr (probably on the scale of difficult to intractible)
//              or we could come up with some other ingenious way to stay in M mode and see if the instruction was compressed.

//     lw x5, 0(x1)        // read the faulting instruction
//     li x1, 3            // check bottom 2 bits of instruction to see if compressed
//     and x5, x5, x1      // mask the other bits
//     beq x5, x1, trapreturn_uncompressed  // if 11, the instruction is return_uncompressed

// trapreturn_compressed:
//     csrr x1, mepc       // get the mepc again
//     addi x1, x1, 2      // add 2 to find the next instruction
//     j trapreturn_specified // and return

// trapreturn_uncompressed:
//     csrr x1, mepc       // get the mepc again    
//     addi x1, x1, 4      // add 4 to find the next instruction

trapreturn_specified_\MODE\():
    // reset the necessary pointers and registers (x1, x5, x6, and the return address going to mepc)
    // so that when we return to a new virtual address, they're all in the right spot as well.

    beqz a1, trapreturn_finished_\MODE\() // either update values, of go to default return address.

    la x5, trap_return_pagetype_table_\MODE\()
    slli a2, a2, 3
    add x5, x5, a2
    ld a2, 0(x5) // a2 = number of offset bits in current page type
    
    li x5, 1
    sll x5, x5, a2
    addi x5, x5, -1 // x5 = mask bits for offset into current pagetype

    // reset the top of the stack, x1
    ld x7, -8(sp) 
    and x7, x5, x7 // x7 = offset for x1
    add x7, x7, a1 // x7 = new address for x1
    sd x7, -8(sp)

    // reset the second spot in the stack, x5
    ld x7, -16(sp)
    and x7, x5, x7 // x7 = offset for x5
    add x7, x7, a1 // x7 = new address for x5
    sd x7, -16(sp)

    // reset x6, the pointer for the virtual address of the output of the tests
    and x7, x5, x6 // x7 = offset for x6
    add x6, x7, a1 // x6 = new address for the result pointer
    
    // set return address, stored temporarily in x1, to the next instruction, but in the new virtual page.
    and x1, x5, x1 // x1 = offset for the return address
    add x1, x1, a1 // x1 = new return address.

    li a1, 0 
    li a2, 0 // reset trapreturn inputs to the trap handler

trapreturn_finished_\MODE\():
    csrw \MODE\()epc, x1       // update the epc with address of next instruction
    ld x5, -16(sp)      // restore registers from stack before returning
    ld x1, -8(sp)
    \MODE\()ret  // return from trap

ecallhandler_\MODE\():
    // Check input parameter a0. encoding above. 
    li x5, 2            // case 2: change to machine mode
    beq a0, x5, ecallhandler_changetomachinemode_\MODE\()
    li x5, 3            // case 3: change to supervisor mode
    beq a0, x5, ecallhandler_changetosupervisormode_\MODE\()
    li x5, 4            // case 4: change to user mode
    beq a0, x5, ecallhandler_changetousermode_\MODE\()
    // unsupported ecalls should segfault
    j segfault_\MODE\()

ecallhandler_changetomachinemode_\MODE\():
    // Force status.MPP (bits 12:11) to 11 to enter machine mode after mret
    li x1, 0b1100000000000
    csrs \MODE\()status, x1
    j trapreturn_\MODE\()        

ecallhandler_changetosupervisormode_\MODE\():
    // Force status.MPP (bits 12:11) to 01 to enter supervisor mode after mret
    li x1, 0b1100000000000  
    csrc \MODE\()status, x1
    li x1, 0b0100000000000
    csrs \MODE\()status, x1
    j trapreturn_\MODE\()

ecallhandler_changetousermode_\MODE\():
    // Force mstatus.MPP (bits 12:11) to 00 to enter user mode after mret
    li x1, 0b1100000000000  
    csrc mstatus, x1
    j trapreturn_\MODE\()

instrfault_\MODE\():
    ld x1, -8(sp) // load return address int x1 (the address AFTER the jal into faulting page)
    j trapreturn_finished_\MODE\() // puts x1 into mepc, restores stack and returns to program (outside of faulting page)

illegalinstr_\MODE\():
    j trapreturn_\MODE\() // return to the code after recording the mcause

accessfault_\MODE\():
    // *** What do I have to do here?
    j trapreturn_\MODE\()

s_soft_interrupt_\MODE\(): // these labels are here to make sure the code compiles, but don't actually do anything yet
    j trapreturn_\MODE\()

m_soft_interrupt_\MODE\():
    j trapreturn_\MODE\()

s_time_interrupt_\MODE\():
    j trapreturn_\MODE\()

m_time_interrupt_\MODE\():
    j trapreturn_\MODE\()

s_ext_interrupt_\MODE\():
    j trapreturn_\MODE\()

m_ext_interrupt_\MODE\():
    j trapreturn_\MODE\()


    // Table of trap behavior
    // lists what to do on each exception (not interrupts)
    // unexpected exceptions should cause segfaults for easy detection
    // Expected exceptions should increment the EPC to the next instruction and return

    .align 3 // aligns this data table to an 8 byte boundary
exception_vector_table_\MODE\():
    .8byte segfault_\MODE\()      // 0: instruction address misaligned
    .8byte instrfault_\MODE\()    // 1: instruction access fault
    .8byte illegalinstr_\MODE\()  // 2: illegal instruction
    .8byte segfault_\MODE\()      // 3: breakpoint
    .8byte segfault_\MODE\()      // 4: load address misaligned
    .8byte accessfault_\MODE\()   // 5: load access fault
    .8byte segfault_\MODE\()      // 6: store address misaligned
    .8byte accessfault_\MODE\()   // 7: store access fault
    .8byte ecallhandler_\MODE\()  // 8: ecall from U-mode
    .8byte ecallhandler_\MODE\()  // 9: ecall from S-mode
    .8byte segfault_\MODE\()      // 10: reserved
    .8byte ecallhandler_\MODE\()  // 11: ecall from M-mode
    .8byte instrfault_\MODE\()    // 12: instruction page fault
    .8byte trapreturn_\MODE\()    // 13: load page fault
    .8byte segfault_\MODE\()      // 14: reserved
    .8byte trapreturn_\MODE\()    // 15: store page fault

.align 3
trap_return_pagetype_table_\MODE\():
    .8byte 0xC  // 0: kilopage has 12 offset bits
    .8byte 0x15 // 1: megapage has 21 offset bits
    .8byte 0x1E // 2: gigapage has 30 offset bits
    .8byte 0x27 // 3: terapage has 39 offset bits

trap_handler_end_\MODE\(): // place to jump to so we can skip the trap handler and continue with the test
.endm

// Test Summary table!

// Test Name            : Description                               : Fault output value                        : Normal output values
// ---------------------:-------------------------------------------:-------------------------------------------:------------------------------------------------------
//   write64_test       : Write 64 bits to address                  : 0x6, 0x7, or 0xf                          : None
//   write32_test       : Write 32 bits to address                  : 0x6, 0x7, or 0xf                          : None 
//   write16_test       : Write 16 bits to address                  : 0x6, 0x7, or 0xf                          : None 
//   write08_test       : Write 8 bits to address                   : 0x6, 0x7, or 0xf                          : None
//   read64_test        : Read 64 bits from address                 : 0x4, 0x5, or 0xd, then 0xbad              : readvalue in hex
//   read32_test        : Read 32 bits from address                  : 0x4, 0x5, or 0xd, then 0xbad              : readvalue in hex
//   read16_test        : Read 16 bits from address                  : 0x4, 0x5, or 0xd, then 0xbad              : readvalue in hex
//   read08_test        : Read 8 bits from address                   : 0x4, 0x5, or 0xd, then 0xbad              : readvalue in hex
//   executable_test    : test executable on virtual page           : 0x0, 0x1, or 0xc, then 0xbad              : value of x7 modified by exectuion code (usually 0x111)
//   terminate_test     : terminate tests                           : mcause value for fault                    : from M 0xb, from S 0x9, from U 0x8  
//   goto_baremetal     : satp.MODE = bare metal                    : None                                      : None 
//   goto_sv39          : satp.MODE = sv39                          : None                                      : None 
//   goto_sv48          : satp.MODE = sv48                          : None                                      : None
//   goto_m_mode        : go to mahcine mode                        : mcause value for fault                    : from M 0xb, from S 0x9, from U 0x8  
//   goto_s_mode        : go to supervisor mode                     : mcause value for fault                    : from M 0xb, from S 0x9, from U 0x8
//   goto_u_mode        : go to user mode                           : mcause value for fault                    : from M 0xb, from S 0x9, from U 0x8 
//   write_read_csr     : write to specified CSR                    : old CSR value, 0x2, depending on perms    : value written to CSR
//   csr_r_access       : test read-only permissions on CSR         : 0xbad                                     : 0x2, then 0x11

// *** TESTS TO ADD: execute inline, read unknown value out, read CSR unknown value, just read CSR value

.macro WRITE64 ADDR VAL
    // attempt to write VAL to ADDR
    // Success outputs:
    //      None
    // Fault outputs:
    //      0x6: misaligned address
    //      0x7: access fault
    //      0xf: page fault     
    li x29, \VAL
    li x30, \ADDR
    sd x29, 0(x30)
.endm

.macro WRITE32 ADDR VAL
    // all write tests have the same description/outputs as write64
    li x29, \VAL
    li x30, \ADDR
    sw x29, 0(x30)
.endm

.macro WRITE16 ADDR VAL
    // all write tests have the same description/outputs as write64
    li x29, \VAL
    li x30, \ADDR
    sh x29, 0(x30)
.endm

.macro WRITE08 ADDR VAL
    // all write tests have the same description/outputs as write64
    li x29, \VAL
    li x30, \ADDR
    sb x29, 0(x30)
.endm

.macro READ64 ADDR
    // Attempt read at ADDR. Write the value read out to the output *** Consider adding specific test for reading a non known value
    // Success outputs:
    //      value read out from ADDR
    // Fault outputs:
    //      One of the following followed by 0xBAD
    //      0x4: misaligned address
    //      0x5: access fault
    //      0xD: page fault
    li x7, 0xBAD // bad value that will be overwritten on good reads.
    li x29, \ADDR 
    ld x7, 0(x29) 
    sd x7, 0(x6)
    addi x6, x6, 8 
    addi x16, x16, 8
.endm

.macro READ32 ADDR
    // All reads have the same description/outputs as read64. 
    // They will store the sign extended value of what was read out at ADDR
    li x7, 0xBAD // bad value that will be overwritten on good reads.
    li x29, \ADDR 
    lw x7, 0(x29) 
    sd x7, 0(x6)
    addi x6, x6, 8 
    addi x16, x16, 8
.endm

.macro READ16 ADDR
    // All reads have the same description/outputs as read64. 
    // They will store the sign extended value of what was read out at ADDR
    li x7, 0xBAD // bad value that will be overwritten on good reads.
    li x29, \ADDR 
    lh x7, 0(x29) 
    sd x7, 0(x6)
    addi x6, x6, 8 
    addi x16, x16, 8
.endm

.macro READ08 ADDR
    // All reads have the same description/outputs as read64. 
    // They will store the sign extended value of what was read out at ADDR
    li x7, 0xBAD // bad value that will be overwritten on good reads.
    li x29, \ADDR 
    lb x7, 0(x29) 
    sd x7, 0(x6)
    addi x6, x6, 8 
    addi x16, x16, 8
.endm

// These goto_x_mode tests all involve invoking the trap handler,
// So their outputs are inevitably:
//      0x8: test called from U mode
//      0x9: test called from S mode
//      0xB: test called from M mode
// they generally do not fault or cause issues as long as these modes are enabled 
// *** add functionality to check if modes are enabled before jumping? maybe cause a fault if not?

.macro GOTO_M_MODE RETURN_VPN RETURN_PAGETYPE
    li a0, 2 // determine trap handler behavior (go to machine mode)
    li a1, \RETURN_VPN // return VPN
    li a2, \RETURN_PAGETYPE // return page types
    ecall // writes mcause to the output.
    // now in S mode
.endm

.macro GOTO_S_MODE RETURN_VPN RETURN_PAGETYPE
    li a0, 3 // determine trap handler behavior (go to supervisor mode)
    li a1, \RETURN_VPN // return VPN
    li a2, \RETURN_PAGETYPE // return page types
    ecall // writes mcause to the output.
    // now in S mode
.endm

.macro GOTO_U_MODE RETURN_VPN RETURN_PAGETYPE
    li a0, 4 // determine trap handler behavior (go to user mode)
    li a1, \RETURN_VPN // return VPN
    li a2, \RETURN_PAGETYPE // return page types
    ecall // writes mcause to the output.
    // now in S mode
.endm

// These tests change virtual memory settings, turning it on/off and changing between types.
// They don't have outputs as any error with turning on virtual memory should reveal itself in the tests *** Consider changing this policy?

.macro GOTO_BAREMETAL
    // Turn translation off
    li x7, 0 // satp.MODE value for bare metal (0)
    slli x7, x7, 60
    csrw satp, x7
.endm

.macro GOTO_SV39 ASID BASE_PPN
    // Turn on sv39 virtual memory
    li x7, 8 // satp.MODE value for Sv39 (8)
    slli x7, x7, 60
    li x29, \ASID
    slli x29, x29, 44
    or x7, x7, x29 // put ASID into the correct field of SATP
    li x28, \BASE_PPN // Base Pagetable physical page number, satp.PPN field.
    add x7, x7, x28
    csrw satp, x7
.endm

.macro GOTO_SV48 ASID BASE_PPN
    // Turn on sv48 virtual memory
    li x7, 9 // satp.MODE value for Sv39 (8)
    slli x7, x7, 60
    li x29, \ASID
    slli x29, x29, 44
    or x7, x7, x29 // put ASID into the correct field of SATP
    li x28, \BASE_PPN // Base Pagetable physical page number, satp.PPN field.
    add x7, x7, x28
    csrw satp, x7
.endm

.macro WRITE_READ_CSR CSR VAL
    // attempt to write CSR with VAL. Note: this also tests read access to CSR
    // Success outputs:
    //      value read back out from CSR after writing
    // Fault outputs:
    //      The previous CSR value before write attempt
    //      *** Most likely 0x2, the mcause for illegal instruction if we don't have write or read access
    li x30, 0xbad // load bad value to be overwritten by csrr
    li x29, \VAL
    csrw \CSR\(), x29
    csrr x30, \CSR
    sd x30, 0(x6)
    addi x6, x6, 8
    addi x16, x16, 8
.endm

.macro CSR_R_ACCESS CSR
    // verify that a csr is accessible to read but not to write
    // Success outputs:
    //      0x2, then
    //      0x11 *** consider changing to something more meaningful
    // Fault outputs:
    //      0xBAD *** consider changing this one as well. in general, do we need the branching if it hould cause an illegal instruction fault? 
    csrr x29, \CSR
    csrwi \CSR\(), 0xA // Attempt to write a 'random' value to the CSR
    csrr x30, \CSR
    bne x30, x29, 1f // 1f represents write_access
    li x30, 0x11 // Write failed, confirming read only permissions.
    j 2f // j r_access_end
1: // w_access (write succeeded, violating read-only)
    li x30, 0xBAD
2: // r_access end
    sd x30, 0(x6)
    addi x6, x6, 8
    addi x16, x16, 8
.endm

.macro EXECUTE_AT_ADDRESS ADDR
    // Execute the code already written to ADDR, returning the value in x7. 
    // *** Note: this test itself doesn't write the code to ADDR because it might be callled at a point where we dont have write access to ADDR
    // Assumes the code modifies x7, usually to become 0x111. 
    // Sample code:  0x11100393 (li x7, 0x111), 0x00008067 (ret)
    // Success outputs:
    //      modified value of x7. (0x111 if you use the sample code)
    // Fault outputs:
    //      One of the following followed by 0xBAD
    //      0x0: misaligned address
    //      0x1: access fault
    //      0xC: page fault
    fence.i // forces caches and main memory to sync so execution code written to ADDR can run.
    li x7, 0xBAD
    li x28, \ADDR
    jalr x28 // jump to executable test code 
    sd x7, 0(x6)
    addi x6, x6, 8
    addi x16, x16, 8 
.endm

.macro END_TESTS
    // invokes one final ecall to return to machine mode then terminates this program, so the output is
    //      0x8: termination called from U mode
    //      0x9: termination called from S mode
    //      0xB: termination called from M mode
    j terminate_test

.endm

	// ---------------------------------------------------------------------------------------------
    // Test Handler
    //
    // This test handler works in a similar wy to the trap handler. It takes in a few things by reading from a table in memory
    // (see test_cases) and performing certain behavior based on them.
    //
    // Input parameters: 
    //
    // x28:
    //     Address input for the test taking place (think: address to read/write, new address to return to, etc...)
    //
    // x29:
    //     Value input for the test taking place (think: value to write, any other extra info needed)
    //
    // x30:
    //     Label for the location of the test that's about to take place
    // ------------------------------------------------------------------------------------------------------------------------------------

.macro INIT_TEST_TABLE // *** Consider renaming this test. to what???

test_loop_setup:
    la x5, test_cases

test_loop:
    ld x28, 0(x5) // fetch test case address
    ld x29, 8(x5) // fetch test case value
    ld x30, 16(x5) // fetch test case flag
    addi x5, x5, 24 // set x5 to next test case

    // x5 has the symbol for a test's location in the assembly
    li x7, 0x1FFFFF 
    and x30, x30, x7 // This program is always on at least a megapage, so this masks out the megapage offset.
    auipc x7, 0x0
    srli x7, x7, 21
    slli x7, x7, 21 // zero out the bottom 21 bits so the megapage offset of the symbol can be placed there
    or x30, x7, x30 // x30 = virtual address of the symbol for this type of test.

    jr x30

// Test Name             : Description                               : Fault output value     : Normal output values
// ----------------------:-------------------------------------------:------------------------:------------------------------------------------------
//   write64_test        : Write 64 bits to address                  : 0xf                    : None
//   write32_test        : Write 32 bits to address                  : 0xf                    : None 
//   write16_test        : Write 16 bits to address                  : 0xf                    : None 
//   write08_test        : Write 8 bits to address                   : 0xf                    : None
//   read64_test         : Read 64 bits from address                 : 0xd, 0xbad             : readvalue in hex
//   read32_test         : Read 32 bitsfrom address                  : 0xd, 0xbad             : readvalue in hex
//   read16_test         : Read 16 bitsfrom address                  : 0xd, 0xbad             : readvalue in hex
//   read08_test         : Read 8 bitsfrom address                   : 0xd, 0xbad             : readvalue in hex
//   executable_test     : test executable on virtual page           : 0xc, 0xbad             : value of x7 modified by exectuion code (usually 0x111)
//   terminate_test      : terminate tests                           : mcause value for fault : from M 0xb, from S 0x9, from U 0x8  
//   goto_baremetal      : satp.MODE = bare metal                    : None                   : None 
//   goto_sv39           : satp.MODE = sv39                          : None                   : None 
//   goto_sv48           : satp.MODE = sv48                          : None                   : None
//   write_mxr_sum       : write sstatus.[19:18] = MXR, SUM bits     : None                   : None
//   goto_m_mode         : go to mahcine mode                        : mcause value for fault : from M 0xb, from S 0x9, from U 0x8  
//   goto_s_mode         : go to supervisor mode                     : mcause value for fault : from M 0xb, from S 0x9, from U 0x8
//   goto_u_mode         : go to user mode                           : mcause value for fault : from M 0xb, from S 0x9, from U 0x8 
//   write_pmpcfg_x      : Write one of the pmpcfg csr's             : mstatuses?, 0xD        : readback of pmpcfg value
//   write_pmpaddr_x     : Write one of the pmpaddr csr's            : None                   : readback of pmpaddr value


write64_test:
    // address to write in x28, double value in x29
    sd x29, 0(x28)
    j test_loop // go to next test case

write32_test:
    // address to write in x28, word value in x29
    sw x29, 0(x28)
    j test_loop // go to next test case

write16_test:
    // address to write in x28, halfword value in x29
    sh x29, 0(x28)
    j test_loop // go to next test case

write08_test:
    // address to write in x28, value in x29
    sb x29, 0(x28)
    j test_loop // go to next test case

read64_test:
    // address to read in x28, expected 64 bit value in x29 (unused, but there for your perusal).
    li x7, 0xBAD // bad value that will be overwritten on good reads.
    ld x7, 0(x28)
    sd x7, 0(x6)
    addi x6, x6, 8
    addi x16, x16, 8
    j test_loop // go to next test case

read32_test:
    // address to read in x28, expected 32 bit value in x29 (unused, but there for your perusal).
    li x7, 0xBAD // bad value that will be overwritten on good reads.
    lw x7, 0(x28)
    sd x7, 0(x6)
    addi x6, x6, 8
    addi x16, x16, 8
    j test_loop // go to next test case

read16_test:
    // address to read in x28, expected 16 bit value in x29 (unused, but there for your perusal).
    li x7, 0xBAD // bad value that will be overwritten on good reads.
    lh x7, 0(x28)
    sd x7, 0(x6)
    addi x6, x6, 8
    addi x16, x16, 8
    j test_loop // go to next test case

read08_test:
    // address to read in x28, expected 8 bit value in x29 (unused, but there for your perusal).
    li x7, 0xBAD // bad value that will be overwritten on good reads.
    lb x7, 0(x28)
    sd x7, 0(x6)
    addi x6, x6, 8
    addi x16, x16, 8
    j test_loop // go to next test case

goto_s_mode:
    // return to address in x28, 
    li a0, 3 // Trap handler behavior (go to supervisor mode)
    mv a1, x28 // return VPN
    mv a2, x29 // return page types
    ecall // writes mcause to the output.
    // now in S mode
    j test_loop

goto_m_mode:
    li a0, 2 // Trap handler behavior (go to machine mode)
    mv a1, x28 // return VPN
    mv a2, x29 // return page types
    ecall // writes mcause to the output.
    j test_loop

goto_u_mode:
    li a0, 4 // Trap handler behavior (go to user mode)
    mv a1, x28 // return VPN
    mv a2, x29 // return page types
    ecall // writes mcause to the output.
    j test_loop

goto_baremetal:
    // Turn translation off
    GOTO_BAREMETAL
    j test_loop // go to next test case

goto_sv39:
    // Turn sv39 translation on
    // Base PPN in x28, ASID in x29
    li x7, 8 // satp.MODE value for sv39 (8)
    slli x7, x7, 60
    slli x29, x29, 44
    or x7, x7, x29 // put ASID into the correct field of SATP
    or x7, x7, x28 // Base Pagetable physical page number, satp.PPN field.
    csrw satp, x7
    li x29, 0xFFFFFFFFFFFFF888
    sfence.vma x0, x29 // just an attempt ***
    j test_loop // go to next test case

goto_sv48:
    // Turn sv48 translation on
    // Base PPN in x28, ASID in x29
    li x7, 9 // satp.MODE value for sv48 (9)
    slli x7, x7, 60
    slli x29, x29, 44
    or x7, x7, x29 // put ASID into the correct field of SATP
    or x7, x7, x28 // Base Pagetable physical page number, satp.PPN field.
    csrw satp, x7
    j test_loop // go to next test case

write_mxr_sum:
    // writes sstatus.[mxr, sum] with the (assumed to be) 2 bit value in x29. also assumes we're in S or M mode
    li x30, 0xC0000 // mask bits for MXR, SUM
    not x7, x29
    slli x7, x7, 18
    and x7, x7, x30
    slli x29, x29, 18
    csrc sstatus, x7
    csrs sstatus, x29
    j test_loop

read_write_mprv:
    // reads old mstatus.mprv value to output, then
    // Writes mstatus.mprv with the 1 bit value in x29. assumes we're in m mode
    li x30, 0x20000 // mask bits for mprv
    csrr x7, mstatus
    and x7, x7, x30
    srli x7, x7, 17
    sd x7, 0(x6) // store old mprv to output
    addi x6, x6, 8
    addi x16, x16, 8 

    not x7, x29
    slli x7, x7, 17
    slli x29, x29, 17
    csrc mstatus, x7
    csrs mstatus, x29 // clear or set mprv bit
    li x7, 0x1800  
    csrc mstatus, x7
    li x7, 0x800
    csrs mstatus, x7 // set mpp to supervisor mode to see if mprv=1 really executes in the mpp mode
    j test_loop


write_pmpcfg_0:
    // writes the value in x29 to the pmpcfg register specified in x28.
    // then writes the final value of pmpcfgX to the output.
    csrw pmpcfg0, x29
    csrr x30, pmpcfg0
    j write_pmpcfg_end

write_pmpcfg_2:
    csrw pmpcfg2, x29
    csrr x30, pmpcfg2 // I would use csrrw but we need the value AFTER the csr has been written
    j write_pmpcfg_end

write_pmpcfg_end:
    sd x30, 0(x6)
    addi x6, x6, 8
    addi x16, x16, 8
    j test_loop

write_pmpaddr_0:
    // write_read_csr pmpaddr0, x29
    // writes the value in x29 to the pmpaddr register specified in x28.
    // then writes the final value of pmpaddrX to the output.
    csrw pmpaddr0, x29
    csrr x30, pmpaddr0
    j write_pmpaddr_end

write_pmpaddr_1:
    csrw pmpaddr1, x29
    csrr x30, pmpaddr1
    j write_pmpaddr_end

write_pmpaddr_2:
    csrw pmpaddr2, x29
    csrr x30, pmpaddr2
    j write_pmpaddr_end

write_pmpaddr_3:
    csrw pmpaddr3, x29
    csrr x30, pmpaddr3
    j write_pmpaddr_end

write_pmpaddr_4:
    csrw pmpaddr4, x29
    csrr x30, pmpaddr4
    j write_pmpaddr_end

write_pmpaddr_5:
    csrw pmpaddr5, x29
    csrr x30, pmpaddr5
    j write_pmpaddr_end

write_pmpaddr_6:
    csrw pmpaddr6, x29
    csrr x30, pmpaddr6
    j write_pmpaddr_end

write_pmpaddr_7:
    csrw pmpaddr7, x29
    csrr x30, pmpaddr7
    j write_pmpaddr_end

write_pmpaddr_8:
    csrw pmpaddr8, x29
    csrr x30, pmpaddr8
    j write_pmpaddr_end

write_pmpaddr_9:
    csrw pmpaddr9, x29
    csrr x30, pmpaddr9
    j write_pmpaddr_end

write_pmpaddr_10:
    csrw pmpaddr10, x29
    csrr x30, pmpaddr10
    j write_pmpaddr_end

write_pmpaddr_11:
    csrw pmpaddr11, x29
    csrr x30, pmpaddr11
    j write_pmpaddr_end

write_pmpaddr_12:
    csrw pmpaddr12, x29
    csrr x30, pmpaddr12
    j write_pmpaddr_end

write_pmpaddr_13:
    csrw pmpaddr13, x29
    csrr x30, pmpaddr13
    j write_pmpaddr_end

write_pmpaddr_14:
    csrw pmpaddr14, x29
    csrr x30, pmpaddr14
    j write_pmpaddr_end

write_pmpaddr_15:
    csrw pmpaddr15, x29
    csrr x30, pmpaddr15
    j write_pmpaddr_end

write_pmpaddr_end:
    sd x30, 0(x6)
    addi x6, x6, 8
    addi x16, x16, 8
    j test_loop

executable_test:
    // Execute the code at the address in x28, returning the value in x7.
    // Assumes the code modifies x7, to become the value stored in x29 for this test.  
    fence.i // forces cache and main memory to sync so execution code written by the program can run.
    li x7, 0xBAD
    jalr x28 
    sd x7, 0(x6) 
    addi x6, x6, 8
    addi x16, x16, 8 
    j test_loop

.endm 

// notably, terminate_test is not a part of the test table macro because it needs to be defined 
// in any type of test, macro or test table, for the trap handler to work
terminate_test:

    li a0, 2 // Trap handler behavior (go to machine mode)
    ecall //  writes mcause to the output.
    csrw mtvec, x4  // restore original trap handler to halt program

RVTEST_CODE_END
RVMODEL_HALT

.macro TEST_STACK_AND_DATA

RVTEST_DATA_BEGIN
.align 4
rvtest_data:
.word 0xbabecafe
RVTEST_DATA_END

.align 3 // align stack to 8 byte boundary
bottom_of_stack:
    .fill 1024, 4, 0xdeadbeef 
top_of_stack:


RVMODEL_DATA_BEGIN

test_1_res:
    .fill 1024, 4, 0xdeadbeef

RVMODEL_DATA_END

#ifdef rvtest_mtrap_routine

mtrap_sigptr:
    .fill 64*(XLEN/32),4,0xdeadbeef

#endif

#ifdef rvtest_gpr_save

gpr_save:
    .fill 32*(XLEN/32),4,0xdeadbeef

#endif

.endm
