/*
 * Copyright (c) 2003, 2020, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2014, 2019, Red Hat Inc. All rights reserved.
 * Copyright (c) 2020, 2022, Huawei Technologies Co., Ltd. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "asm/macroAssembler.hpp"
#include "asm/macroAssembler.inline.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/barrierSetAssembler.hpp"
#include "interpreter/interpreter.hpp"
#include "nativeInst_riscv.hpp"
#include "oops/instanceOop.hpp"
#include "oops/method.hpp"
#include "oops/objArrayKlass.hpp"
#include "oops/oop.inline.hpp"
#include "prims/methodHandles.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubCodeGenerator.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/thread.inline.hpp"
#include "utilities/align.hpp"
#ifdef COMPILER2
#include "opto/runtime.hpp"
#endif


// Declaration and definition of StubGenerator (no .hpp file).
// For a more detailed description of the stub routine structure
// see the comment in stubRoutines.hpp

#undef __
#define __ _masm->

#ifdef PRODUCT
#define BLOCK_COMMENT(str) /* nothing */
#else
#define BLOCK_COMMENT(str) __ block_comment(str)
#endif

#define BIND(label) bind(label); BLOCK_COMMENT(#label ":")

// Stub Code definitions

class StubGenerator: public StubCodeGenerator {
 private:

#ifdef PRODUCT
#define inc_counter_np(counter) ((void)0)
#else
  void inc_counter_np_(int& counter) {
    __ la(t1, ExternalAddress((address)&counter));
    __ lwu(t0, Address(t1, 0));
    __ addiw(t0, t0, 1);
    __ sw(t0, Address(t1, 0));
  }
#define inc_counter_np(counter) \
  BLOCK_COMMENT("inc_counter " #counter); \
  inc_counter_np_(counter);
#endif

  // Call stubs are used to call Java from C
  //
  // Arguments:
  //    c_rarg0:   call wrapper address                   address
  //    c_rarg1:   result                                 address
  //    c_rarg2:   result type                            BasicType
  //    c_rarg3:   method                                 Method*
  //    c_rarg4:   (interpreter) entry point              address
  //    c_rarg5:   parameters                             intptr_t*
  //    c_rarg6:   parameter size (in words)              int
  //    c_rarg7:   thread                                 Thread*
  //
  // There is no return from the stub itself as any Java result
  // is written to result
  //
  // we save x1 (ra) as the return PC at the base of the frame and
  // link x8 (fp) below it as the frame pointer installing sp (x2)
  // into fp.
  //
  // we save x10-x17, which accounts for all the c arguments.
  //
  // TODO: strictly do we need to save them all? they are treated as
  // volatile by C so could we omit saving the ones we are going to
  // place in global registers (thread? method?) or those we only use
  // during setup of the Java call?
  //
  // we don't need to save x5 which C uses as an indirect result location
  // return register.
  //
  // we don't need to save x6-x7 and x28-x31 which both C and Java treat as
  // volatile
  //
  // we save x18-x27 which Java uses as temporary registers and C
  // expects to be callee-save
  //
  // so the stub frame looks like this when we enter Java code
  //
  //     [ return_from_Java     ] <--- sp
  //     [ argument word n      ]
  //      ...
  // -23 [ argument word 1      ]
  // -22 [ saved x27            ] <--- sp_after_call
  // -21 [ saved x26            ]
  // -20 [ saved x25            ]
  // -19 [ saved x24            ]
  // -18 [ saved x23            ]
  // -17 [ saved x22            ]
  // -16 [ saved x21            ]
  // -15 [ saved x20            ]
  // -14 [ saved x19            ]
  // -13 [ saved x18            ]
  // -12 [ saved x9             ]
  // -11 [ thread pointer (x4)  ]
  // -10 [ call wrapper   (x10) ]
  //  -9 [ result         (x11) ]
  //  -8 [ result type    (x12) ]
  //  -7 [ method         (x13) ]
  //  -6 [ entry point    (x14) ]
  //  -5 [ parameters     (x15) ]
  //  -4 [ parameter size (x16) ]
  //  -3 [ thread         (x17) ]
  //  -2 [ saved fp       (x8)  ] 
  //  -1 [ saved ra       (x1)  ] 
  //   0 [                      ] <--- fp == saved sp (x2)

  // Call stub stack layout word offsets from fp
  enum call_stub_layout {
    sp_after_call_off  = -22,

    x27_off            = -22,
    x26_off            = -21,
    x25_off            = -20,
    x24_off            = -19,
    x23_off            = -18,
    x22_off            = -17,
    x21_off            = -16,
    x20_off            = -15,
    x19_off            = -14,
    x18_off            = -13,
    x9_off             = -12,

    x4_off             = -11,

    call_wrapper_off   = -10,
    result_off         =  -9,
    result_type_off    =  -8,
    method_off         =  -7,
    entry_point_off    =  -6,
    parameters_off     =  -5,
    parameter_size_off =  -4,
    thread_off         =  -3,
    fp_f               =  -2,
    retaddr_off        =  -1,
  };

  address generate_call_stub(address& return_address) {
    assert((int)frame::entry_frame_after_call_words == -(int)sp_after_call_off + 1 &&
           (int)frame::entry_frame_call_wrapper_offset == (int)call_wrapper_off,
           "adjust this code");

    StubCodeMark mark(this, "StubRoutines", "call_stub");
    address start = __ pc();

    const Address sp_after_call (fp, sp_after_call_off  * wordSize);

    const Address call_wrapper  (fp, call_wrapper_off   * wordSize);
    const Address result        (fp, result_off         * wordSize);
    const Address result_type   (fp, result_type_off    * wordSize);
    const Address method        (fp, method_off         * wordSize);
    const Address entry_point   (fp, entry_point_off    * wordSize);
    const Address parameters    (fp, parameters_off     * wordSize);
    const Address parameter_size(fp, parameter_size_off * wordSize);

    const Address thread        (fp, thread_off         * wordSize);

    const Address x27_save      (fp, x27_off            * wordSize);
    const Address x26_save      (fp, x26_off            * wordSize);
    const Address x25_save      (fp, x25_off            * wordSize);
    const Address x24_save      (fp, x24_off            * wordSize);
    const Address x23_save      (fp, x23_off            * wordSize);
    const Address x22_save      (fp, x22_off            * wordSize);
    const Address x21_save      (fp, x21_off            * wordSize);
    const Address x20_save      (fp, x20_off            * wordSize);
    const Address x19_save      (fp, x19_off            * wordSize);
    const Address x18_save      (fp, x18_off            * wordSize);

    const Address x9_save       (fp, x9_off             * wordSize);
    const Address x4_save       (fp, x4_off             * wordSize);

    // stub code

    address riscv64_entry = __ pc();

    // set up frame and move sp to end of save area
    __ enter();
    __ addi(sp, fp, sp_after_call_off * wordSize);

    // save register parameters and Java temporary/global registers
    // n.b. we save thread even though it gets installed in
    // xthread because we want to sanity check tp later
    __ sd(c_rarg7, thread);
    __ sw(c_rarg6, parameter_size);
    __ sd(c_rarg5, parameters);
    __ sd(c_rarg4, entry_point);
    __ sd(c_rarg3, method);
    __ sd(c_rarg2, result_type);
    __ sd(c_rarg1, result);
    __ sd(c_rarg0, call_wrapper);

    __ sd(x4, x4_save);
    __ sd(x9, x9_save);

    __ sd(x18, x18_save);
    __ sd(x19, x19_save);
    __ sd(x20, x20_save);
    __ sd(x21, x21_save);
    __ sd(x22, x22_save);
    __ sd(x23, x23_save);
    __ sd(x24, x24_save);
    __ sd(x25, x25_save);
    __ sd(x26, x26_save);
    __ sd(x27, x27_save);

    // install Java thread in global register now we have saved
    // whatever value it held
    __ mv(xthread, c_rarg7);

    // And method
    __ mv(xmethod, c_rarg3);

    // set up the heapbase register
    __ reinit_heapbase();

#ifdef ASSERT
    // make sure we have no pending exceptions
    {
      Label L;
      __ ld(t0, Address(xthread, in_bytes(Thread::pending_exception_offset())));
      __ beqz(t0, L);
      __ stop("StubRoutines::call_stub: entered with pending exception");
      __ BIND(L);
    }
#endif
    // pass parameters if any
    __ mv(esp, sp);
    __ slli(t0, c_rarg6, LogBytesPerWord);
    __ sub(t0, sp, t0); // Move SP out of the way
    __ andi(sp, t0, -2 * wordSize);

    BLOCK_COMMENT("pass parameters if any");
    Label parameters_done;
    // parameter count is still in c_rarg6
    // and parameter pointer identifying param 1 is in c_rarg5
    __ beqz(c_rarg6, parameters_done);

    address loop = __ pc();
    __ ld(t0, c_rarg5, 0);
    __ addi(c_rarg5, c_rarg5, wordSize);
    __ addi(c_rarg6, c_rarg6, -1);
    __ push_reg(t0);
    __ bgtz(c_rarg6, loop);

    __ BIND(parameters_done);

    // call Java entry -- passing methdoOop, and current sp
    //      xmethod: Method*
    //      x30: sender sp
    BLOCK_COMMENT("call Java function");
    __ mv(x30, sp);
    __ jalr(c_rarg4);

    // save current address for use by exception handling code

    return_address = __ pc();

    // store result depending on type (everything that is not
    // T_OBJECT, T_LONG, T_FLOAT or T_DOUBLE is treated as T_INT)
    // n.b. this assumes Java returns an integral result in x10
    // and a floating result in j_farg0
    __ ld(j_rarg2, result);
    Label is_long, is_float, is_double, exit;
    __ ld(j_rarg1, result_type);
    __ li(t0, (u1)T_OBJECT);
    __ beq(j_rarg1, t0, is_long);
    __ li(t0, (u1)T_LONG);
    __ beq(j_rarg1, t0, is_long);
    __ li(t0, (u1)T_FLOAT);
    __ beq(j_rarg1, t0, is_float);
    __ li(t0, (u1)T_DOUBLE);
    __ beq(j_rarg1, t0, is_double);

    // handle T_INT case
    __ sw(x10, Address(j_rarg2));

    __ BIND(exit);

    // pop parameters
    __ addi(esp, fp, sp_after_call_off * wordSize);

#ifdef ASSERT
    // verify that threads correspond
    {
      Label L, S;
      __ ld(t0, thread);
      __ bne(xthread, t0, S);
      __ get_thread(t0);
      __ beq(xthread, t0, L);
      __ BIND(S);
      __ stop("StubRoutines::call_stub: threads must correspond");
      __ BIND(L);
    }
#endif

    // restore callee-save registers
    __ ld(x27, x27_save);
    __ ld(x26, x26_save);
    __ ld(x25, x25_save);
    __ ld(x24, x24_save);
    __ ld(x23, x23_save);
    __ ld(x22, x22_save);
    __ ld(x21, x21_save);
    __ ld(x20, x20_save);
    __ ld(x19, x19_save);
    __ ld(x18, x18_save);

    __ ld(x9, x9_save);
    __ ld(x4, x4_save);

    __ ld(c_rarg0, call_wrapper);
    __ ld(c_rarg1, result);
    __ ld(c_rarg2, result_type);
    __ ld(c_rarg3, method);
    __ ld(c_rarg4, entry_point);
    __ ld(c_rarg5, parameters);
    __ ld(c_rarg6, parameter_size);
    __ ld(c_rarg7, thread);

    // leave frame and return to caller
    __ leave();
    __ ret();

    // handle return types different from T_INT

    __ BIND(is_long);
    __ sd(x10, Address(j_rarg2, 0));
    __ j(exit);

    __ BIND(is_float);
    __ fsw(j_farg0, Address(j_rarg2, 0), t0);
    __ j(exit);

    __ BIND(is_double);
    __ fsd(j_farg0, Address(j_rarg2, 0), t0);
    __ j(exit);

    return start;
  }

  // Return point for a Java call if there's an exception thrown in
  // Java code.  The exception is caught and transformed into a
  // pending exception stored in JavaThread that can be tested from
  // within the VM.
  //
  // Note: Usually the parameters are removed by the callee. In case
  // of an exception crossing an activation frame boundary, that is
  // not the case if the callee is compiled code => need to setup the
  // rsp.
  //
  // x10: exception oop

  address generate_catch_exception() {
    StubCodeMark mark(this, "StubRoutines", "catch_exception");
    address start = __ pc();

    // same as in generate_call_stub():
    const Address thread(fp, thread_off * wordSize);

#ifdef ASSERT
    // verify that threads correspond
    {
      Label L, S;
      __ ld(t0, thread);
      __ bne(xthread, t0, S);
      __ get_thread(t0);
      __ beq(xthread, t0, L);
      __ bind(S);
      __ stop("StubRoutines::catch_exception: threads must correspond");
      __ bind(L);
    }
#endif

    // set pending exception
    __ verify_oop(x10);

    __ sd(x10, Address(xthread, Thread::pending_exception_offset()));
    __ mv(t0, (address)__FILE__);
    __ sd(t0, Address(xthread, Thread::exception_file_offset()));
    __ mv(t0, (int)__LINE__);
    __ sw(t0, Address(xthread, Thread::exception_line_offset()));

    // complete return to VM
    assert(StubRoutines::_call_stub_return_address != NULL,
           "_call_stub_return_address must have been generated before");
    __ j(StubRoutines::_call_stub_return_address);

    return start;
  }

  // Continuation point for runtime calls returning with a pending
  // exception.  The pending exception check happened in the runtime
  // or native call stub.  The pending exception in Thread is
  // converted into a Java-level exception.
  //
  // Contract with Java-level exception handlers:
  // x10: exception
  // x13: throwing pc
  //
  // NOTE: At entry of this stub, exception-pc must be in RA !!

  // NOTE: this is always used as a jump target within generated code
  // so it just needs to be generated code with no x86 prolog

  address generate_forward_exception() {
    StubCodeMark mark(this, "StubRoutines", "forward exception");
    address start = __ pc();

    // Upon entry, RA points to the return address returning into
    // Java (interpreted or compiled) code; i.e., the return address
    // becomes the throwing pc.
    //
    // Arguments pushed before the runtime call are still on the stack
    // but the exception handler will reset the stack pointer ->
    // ignore them.  A potential result in registers can be ignored as
    // well.

#ifdef ASSERT
    // make sure this code is only executed if there is a pending exception
    {
      Label L;
      __ ld(t0, Address(xthread, Thread::pending_exception_offset()));
      __ bnez(t0, L);
      __ stop("StubRoutines::forward exception: no pending exception (1)");
      __ bind(L);
    }
#endif

    // compute exception handler into x9

    // call the VM to find the handler address associated with the
    // caller address. pass thread in x10 and caller pc (ret address)
    // in x11. n.b. the caller pc is in ra, unlike x86 where it is on
    // the stack.
    __ mv(c_rarg1, ra);
    // ra will be trashed by the VM call so we move it to x9
    // (callee-saved) because we also need to pass it to the handler
    // returned by this call.
    __ mv(x9, ra);
    BLOCK_COMMENT("call exception_handler_for_return_address");
    __ call_VM_leaf(CAST_FROM_FN_PTR(address,
                         SharedRuntime::exception_handler_for_return_address),
                    xthread, c_rarg1);
    // we should not really care that ra is no longer the callee
    // address. we saved the value the handler needs in x9 so we can
    // just copy it to x13. however, the C2 handler will push its own
    // frame and then calls into the VM and the VM code asserts that
    // the PC for the frame above the handler belongs to a compiled
    // Java method. So, we restore ra here to satisfy that assert.
    __ mv(ra, x9);
    // setup x10 & x13 & clear pending exception
    __ mv(x13, x9);
    __ mv(x9, x10);
    __ ld(x10, Address(xthread, Thread::pending_exception_offset()));
    __ sd(zr, Address(xthread, Thread::pending_exception_offset()));

#ifdef ASSERT
    // make sure exception is set
    {
      Label L;
      __ bnez(x10, L);
      __ stop("StubRoutines::forward exception: no pending exception (2)");
      __ bind(L);
    }
#endif

    // continue at exception handler
    // x10: exception
    // x13: throwing pc
    // x9: exception handler
    __ verify_oop(x10);
    __ jr(x9);

    return start;
  }

  // Non-destructive plausibility checks for oops
  //
  // Arguments:
  //    x10: oop to verify
  //    t0: error message
  //
  // Stack after saving c_rarg3:
  //    [tos + 0]: saved c_rarg3
  //    [tos + 1]: saved c_rarg2
  //    [tos + 2]: saved ra
  //    [tos + 3]: saved t1
  //    [tos + 4]: saved x10
  //    [tos + 5]: saved t0
  address generate_verify_oop() {

    StubCodeMark mark(this, "StubRoutines", "verify_oop");
    address start = __ pc();

    Label exit, error;

    __ push_reg(RegSet::of(c_rarg2, c_rarg3), sp);   // save c_rarg2 and c_rarg3

    __ la(c_rarg2, ExternalAddress((address) StubRoutines::verify_oop_count_addr()));
    __ ld(c_rarg3, Address(c_rarg2));
    __ add(c_rarg3, c_rarg3, 1);
    __ sd(c_rarg3, Address(c_rarg2));

    // object is in x10
    // make sure object is 'reasonable'
    __ beqz(x10, exit); // if obj is NULL it is OK

    // Check if the oop is in the right area of memory
    __ mv(c_rarg3, (intptr_t) Universe::verify_oop_mask());
    __ andr(c_rarg2, x10, c_rarg3);
    __ mv(c_rarg3, (intptr_t) Universe::verify_oop_bits());

    // Compare c_rarg2 and c_rarg3
    __ bne(c_rarg2, c_rarg3, error);

    // make sure klass is 'reasonable', which is not zero.
    __ load_klass(x10, x10);  // get klass
    __ beqz(x10, error);      // if klass is NULL it is broken

    // return if everything seems ok
    __ bind(exit);

    __ pop_reg(RegSet::of(c_rarg2, c_rarg3), sp);   // pop c_rarg2 and c_rarg3
    __ ret();

    // handle errors
    __ bind(error);
    __ pop_reg(RegSet::of(c_rarg2, c_rarg3), sp);   // pop c_rarg2 and c_rarg3

    __ push_reg(RegSet::range(x0, x31), sp);
    // prepare parameters for debug64, c_rarg0: address of error message,
    // c_rarg1: return address, c_rarg2: address of regs on stack
    __ mv(c_rarg0, t0);             // pass address of error message
    __ mv(c_rarg1, ra);             // pass return address
    __ mv(c_rarg2, sp);             // pass address of regs on stack
#ifndef PRODUCT
    assert(frame::arg_reg_save_area_bytes == 0, "not expecting frame reg save area");
#endif
    BLOCK_COMMENT("call MacroAssembler::debug");
    int32_t offset = 0;
    __ movptr_with_offset(t0, CAST_FROM_FN_PTR(address, MacroAssembler::debug64), offset);
    __ jalr(x1, t0, offset);

    return start;
  }

  // The inner part of zero_words().
  //
  // Inputs:
  // x28: the HeapWord-aligned base address of an array to zero.
  // x29: the count in HeapWords, x29 > 0.
  //
  // Returns x28 and x29, adjusted for the caller to clear.
  // x28: the base address of the tail of words left to clear.
  // x29: the number of words in the tail.
  //      x29 < MacroAssembler::zero_words_block_size.

  address generate_zero_blocks() {
    Label done;

    const Register base = x28, cnt = x29;

    __ align(CodeEntryAlignment);
    StubCodeMark mark(this, "StubRoutines", "zero_blocks");
    address start = __ pc();

    {
      // Clear the remaining blocks.
      Label loop;
      __ sub(cnt, cnt, MacroAssembler::zero_words_block_size);
      __ bltz(cnt, done);
      __ bind(loop);
      for (int i = 0; i < MacroAssembler::zero_words_block_size; i++) {
        __ sd(zr, Address(base, 0));
        __ add(base, base, 8);
      }
      __ sub(cnt, cnt, MacroAssembler::zero_words_block_size);
      __ bgez(cnt, loop);
      __ bind(done);
      __ add(cnt, cnt, MacroAssembler::zero_words_block_size);
    }

    __ ret();

    return start;
  }

  typedef void (MacroAssembler::*copy_insn)(Register R1, Register R2, const int32_t offset);

  void copy_by_step(RegSet tmp_regs, Register src, Register dst,
                    unsigned unroll_factor, int unit) {
    unsigned char regs[32];
    int offset = unit < 0 ? unit : 0;

    // Scan bitset to get tmp regs
    unsigned int regsSize = 0;
    unsigned bitset = tmp_regs.bits();

    assert(((bitset & (1 << (src->encoding()))) == 0), "src should not in tmp regs");
    assert(((bitset & (1 << (dst->encoding()))) == 0), "dst should not in tmp regs");

    for (int reg = 31; reg >= 0; reg--) {
      if ((1U << 31) & bitset) {
        regs[regsSize++] = reg;
      }
      bitset <<= 1;
    }

    copy_insn ld_arr = NULL, st_arr = NULL;
    switch (abs(unit)) {
      case 1 :
        ld_arr = (copy_insn)&MacroAssembler::lbu;
        st_arr = (copy_insn)&MacroAssembler::sb;
        break;
      case BytesPerShort :
        ld_arr = (copy_insn)&MacroAssembler::lhu;
        st_arr = (copy_insn)&MacroAssembler::sh;
        break;
      case BytesPerInt :
        ld_arr = (copy_insn)&MacroAssembler::lwu;
        st_arr = (copy_insn)&MacroAssembler::sw;
        break;
      case BytesPerLong :
        ld_arr = (copy_insn)&MacroAssembler::ld;
        st_arr = (copy_insn)&MacroAssembler::sd;
        break;
      default :
        ShouldNotReachHere();
    }

    for (unsigned i = 0; i < unroll_factor; i++) {
      (_masm->*ld_arr)(as_Register(regs[0]), src, i * unit + offset);
      (_masm->*st_arr)(as_Register(regs[0]), dst, i * unit + offset);
    }

    __ addi(src, src, unroll_factor * unit);
    __ addi(dst, dst, unroll_factor * unit);
  }

  void copy_tail(Register src, Register dst, Register count_in_bytes, Register tmp,
                 int ele_size, unsigned align_unit) {
    bool is_backwards = ele_size < 0;
    size_t granularity = uabs(ele_size);
    for (unsigned unit = (align_unit >> 1); unit >= granularity; unit >>= 1) {
      int offset = is_backwards ? (int)(-unit) : unit;
      Label exit;
      __ andi(tmp, count_in_bytes, unit);
      __ beqz(tmp, exit);
      copy_by_step(RegSet::of(tmp), src, dst, /* unroll_factor */ 1, offset);
      __ bind(exit);
    }
  }

  void copy_loop8(Register src, Register dst, Register count_in_bytes, Register tmp,
                  int step, Label *Lcopy_small, Register loopsize = noreg) {
    size_t granularity = uabs(step);
    RegSet tmp_regs = RegSet::range(x13, x16);
    assert_different_registers(src, dst, count_in_bytes, tmp);

    Label loop, copy2, copy1, finish;
    if (loopsize == noreg) {
      loopsize = t1;
      __ li(loopsize, 8 * granularity);
    }

    // Cyclic copy with 8*step.
    __ bind(loop);
    {
      copy_by_step(tmp_regs, src, dst, /* unroll_factor */ 8, step);
      __ sub(count_in_bytes, count_in_bytes, 8 * granularity);
      __ bge(count_in_bytes, loopsize, loop);
    }

    if (Lcopy_small != NULL) {
      __ bind(*Lcopy_small);
    }

    // copy memory smaller than step * 8 bytes
    __ andi(tmp, count_in_bytes, granularity << 2);
    __ beqz(tmp, copy2);
    copy_by_step(tmp_regs, src, dst, /* unroll_factor */ 4, step);

    __ bind(copy2);
    __ andi(tmp, count_in_bytes, granularity << 1);
    __ beqz(tmp, copy1);
    copy_by_step(tmp_regs, src, dst, /* unroll_factor */ 2, step);

    __ bind(copy1);
    __ andi(tmp, count_in_bytes, granularity);
    __ beqz(tmp, finish);
    copy_by_step(tmp_regs, src, dst, /* unroll_factor */ 1, step);

    __ bind(finish);
  }

  // Cyclic copy with one step.
  void copy_loop1(Register src, Register dst, Register count_in_bytes, int step, Register loopsize = noreg) {
    size_t granularity = uabs(step);
    Label loop1;
    if (loopsize == noreg) {
      loopsize = t0;
      __ li(loopsize, granularity);
    }

    __ bind(loop1);
    {
      copy_by_step(RegSet::of(x13), src, dst, /* unroll_factor */ 1, step);
      __ sub(count_in_bytes, count_in_bytes, granularity);
      __ bge(count_in_bytes, loopsize, loop1);
    }
  }

  void align_unit(Register src, Register dst, Register count_in_bytes,
                  unsigned unit, bool is_backwards) {
    Label skip;
    __ andi(t0, dst, unit);
    __ beqz(t0, skip);
    copy_by_step(RegSet::of(t0), src, dst, 1, is_backwards ? -unit : unit);
    __ sub(count_in_bytes, count_in_bytes, unit);
    __ bind(skip);
  }

  void copy_memory(bool is_align, Register s, Register d, Register count_in_elements,
                   Register tmp, int ele_step) {

    bool is_backwards = ele_step < 0;
    unsigned int granularity = uabs(ele_step);
    Label Lcopy_small, Ldone, Lcopy_ele, Laligned;
    const Register count_in_bytes = x31, src = x28, dst = x29;
    assert_different_registers(src, dst, count_in_elements, count_in_bytes, tmp, t1);
    __ slli(count_in_bytes, count_in_elements, exact_log2(granularity));
    __ add(src, s, is_backwards ? count_in_bytes : zr);
    __ add(dst, d, is_backwards ? count_in_bytes : zr);

    // if count_in_elements < 8, copy_small
    __ li(t0, 8);
    if (is_align && granularity < BytesPerLong) {
      __ blt(count_in_bytes, t0, Lcopy_small);
    } else {
      __ blt(count_in_elements, t0, Lcopy_small);
    }

    if (granularity < BytesPerLong) {
      Label Lcopy_aligned[3];
      Label Lalign8;
      if (!is_align) {
        Label Lalign_and_copy;
        __ li(t0, EagerArrayCopyThreshold);
        __ blt(count_in_bytes, t0, Lalign_and_copy);
        // Align dst to 8.
        for (unsigned unit = granularity; unit <= 4; unit <<= 1) {
          align_unit(src, dst, count_in_bytes, unit, is_backwards);
        }

        Register shr = x30, shl = x7, tmp1 = x13;

        __ andi(shr, src, 0x7);
        __ beqz(shr, Lalign8);
        {
          // calculaute the shift for store doubleword
          __ slli(shr, shr, 3);
          __ sub(shl, shr, 64);
          __ sub(shl, zr, shl);

          // alsrc: previous position of src octal alignment
          Register alsrc = t1;
          __ andi(alsrc, src, -8);

          // move src to tail
          __ andi(t0, count_in_bytes, -8);
          if (is_backwards) {
            __ sub(src, src, t0);
          } else {
            __ add(src, src, t0);
          }

          // prepare for copy_dstaligned_loop
          __ ld(tmp1, alsrc, 0);
          dst_aligned_copy_32bytes_loop(alsrc, dst, shr, shl, count_in_bytes, is_backwards);
          __ li(x17, 8);
          __ blt(count_in_bytes, x17, Lcopy_small);
          dst_aligned_copy_8bytes_loop(alsrc, dst, shr, shl, count_in_bytes, x17, is_backwards);
          __ j(Lcopy_small);
        }
        __ j(Ldone);
        __ bind(Lalign_and_copy);

        // Check src and dst could be 8/4/2 algined at the same time. If could, align the
        // memory and copy by 8/4/2.
        __ xorr(t1, src, dst);

        for (unsigned alignment = granularity << 1; alignment <= 8; alignment <<= 1) {
          Label skip;
          unsigned int unit = alignment >> 1;
          // Check src and dst could be aligned to checkbyte at the same time
          // if copy from src to dst. If couldn't, jump to label not_aligned.
          __ andi(t0, t1, alignment - 1);
          __ bnez(t0, Lcopy_aligned[exact_log2(unit)]);
          // Align src and dst to unit.
          align_unit(src, dst, count_in_bytes, unit, is_backwards);
        }
      }
      __ bind(Lalign8);
      for (unsigned step_size = 8; step_size > granularity; step_size >>= 1) {
        // Copy memory by steps, which has been aligned to step_size.
        Label loop8, Ltail;
        int step = is_backwards ? (-step_size) : step_size;
        if (!(step_size == 8 && is_align)) { // which has load 8 to t0 before
          // Check whether the memory size is smaller than step_size.
          __ li(t0, step_size);
          __ blt(count_in_bytes, t0, Ltail);
        }
        const Register eight_step = t1;
        __ li(eight_step, step_size * 8);
        __ bge(count_in_bytes, eight_step, loop8);
        // If memory is less than 8*step_size bytes, loop by step.
        copy_loop1(src, dst, count_in_bytes, step, t0);
        copy_tail(src, dst, count_in_bytes, tmp, ele_step, step_size);
        __ j(Ldone);

        __ bind(loop8);
        // If memory is greater than or equal to 8*step_size bytes, loop by step*8.
        copy_loop8(src, dst, count_in_bytes, tmp, step, NULL, eight_step);
        __ bind(Ltail);
        copy_tail(src, dst, count_in_bytes, tmp, ele_step, step_size);
        __ j(Ldone);

        __ bind(Lcopy_aligned[exact_log2(step_size >> 1)]);
      }
    }
    // If the ele_step is greater than 8, or the memory src and dst cannot
    // be aligned with a number greater than the value of step.
    // Cyclic copy with 8*ele_step.
    copy_loop8(src, dst, count_in_bytes, tmp, ele_step, &Lcopy_small, noreg);

    __ bind(Ldone);
  }

  void dst_aligned_copy_32bytes_loop(Register alsrc, Register dst,
                                     Register shr,   Register shl,
                                     Register count_in_bytes, bool is_backwards) {
    const Register tmp1 = x13, tmp2 = x14, tmp3 = x15, tmp4 = x16, thirty_two = x17;
    const Register sll_reg1 = is_backwards ? tmp1 : tmp2,
                   srl_reg1 = is_backwards ? tmp2 : tmp1,
                   sll_reg2 = is_backwards ? tmp2 : tmp3,
                   srl_reg2 = is_backwards ? tmp3 : tmp2,
                   sll_reg3 = is_backwards ? tmp3 : tmp4,
                   srl_reg3 = is_backwards ? tmp4 : tmp3,
                   sll_reg4 = is_backwards ? tmp4 : tmp1,
                   srl_reg4 = is_backwards ? tmp1 : tmp4;
    assert_different_registers(t0, thirty_two, alsrc, shr, shl);
    int unit = is_backwards ? -wordSize : wordSize;
    int offset = is_backwards ? -wordSize : 0;
    Label loop, done;

    __ li(thirty_two, 32);
    __ blt(count_in_bytes, thirty_two, done);

    __ bind(loop);
    __ ld(tmp2, alsrc, unit);
    __ sll(t0, sll_reg1, shl);
    __ srl(tmp1, srl_reg1, shr);
    __ orr(tmp1, tmp1, t0);
    __ sd(tmp1, dst, offset);

    __ ld(tmp3, alsrc, unit * 2);
    __ sll(t0, sll_reg2, shl);
    __ srl(tmp2, srl_reg2, shr);
    __ orr(tmp2, tmp2, t0);
    __ sd(tmp2, dst, unit + offset);

    __ ld(tmp4, alsrc, unit * 3);
    __ sll(t0, sll_reg3, shl);
    __ srl(tmp3, srl_reg3, shr);
    __ orr(tmp3, tmp3, t0);
    __ sd(tmp3, dst, unit * 2 + offset);

    __ ld(tmp1, alsrc, unit * 4);
    __ sll(t0, sll_reg4, shl);
    __ srl(tmp4, srl_reg4, shr);
    __ orr(tmp4, tmp4, t0);
    __ sd(tmp4, dst, unit * 3 + offset);

    __ add(alsrc, alsrc, unit * 4);
    __ add(dst, dst, unit * 4);
    __ sub(count_in_bytes, count_in_bytes, 32);
    __ bge(count_in_bytes, thirty_two, loop);

    __ bind(done);
  }

  void dst_aligned_copy_8bytes_loop(Register alsrc, Register dst,
                                    Register shr,   Register shl,
                                    Register count_in_bytes, Register eight,
                                    bool is_backwards) {
    const Register tmp1 = x13, tmp2 = x14, tmp3 = x15, tmp4 = x16;
    const Register sll_reg = is_backwards ? tmp1 : tmp2,
                   srl_reg = is_backwards ? tmp2 : tmp1;
    assert_different_registers(t0, eight, alsrc, shr, shl);
    Label loop;
    int unit = is_backwards ? -wordSize : wordSize;

    __ bind(loop);
    __ ld(tmp2, alsrc, unit);
    __ sll(t0, sll_reg, shl);
    __ srl(tmp1, srl_reg, shr);
    __ orr(t0, tmp1, t0);
    __ sd(t0, dst, is_backwards ? unit : 0);
    __ mv(tmp1, tmp2);
    __ add(alsrc, alsrc, unit);
    __ add(dst, dst, unit);
    __ sub(count_in_bytes, count_in_bytes, 8);
    __ bge(count_in_bytes, eight, loop);
  }

  // Scan over array at a for count oops, verifying each one.
  // Preserves a and count, clobbers t0 and t1.
  void verify_oop_array(int size, Register a, Register count, Register temp) {
    Label loop, end;
    __ mv(t1, zr);
    __ slli(t0, count, exact_log2(size));
    __ bind(loop);
    __ bgeu(t1, t0, end);

    __ add(temp, a, t1);
    if (size == wordSize) {
      __ ld(temp, Address(temp, 0));
      __ verify_oop(temp);
    } else {
      __ lwu(temp, Address(temp, 0));
      __ decode_heap_oop(temp); // calls verify_oop
    }
    __ add(t1, t1, size);
    __ j(loop);
    __ bind(end);
  }

  // Arguments:
  //   aligned - true => Input and output aligned on a HeapWord == 8-byte boundary
  //             ignored
  //   is_oop  - true => oop array, so generate store check code
  //   name    - stub name string
  //
  // Inputs:
  //   c_rarg0   - source array address
  //   c_rarg1   - destination array address
  //   c_rarg2   - element count, treated as ssize_t, can be zero
  //
  // If 'from' and/or 'to' are aligned on 4-byte boundaries, we let
  // the hardware handle it.  The two dwords within qwords that span
  // cache line boundaries will still be loaded and stored atomically.
  //
  // Side Effects:
  //   disjoint_int_copy_entry is set to the no-overlap entry point
  //   used by generate_conjoint_int_oop_copy().
  //
  address generate_disjoint_copy(size_t size, bool aligned, bool is_oop, address* entry,
                                 const char* name, bool dest_uninitialized = false) {
    const Register s = c_rarg0, d = c_rarg1, count = c_rarg2;
    RegSet saved_reg = RegSet::of(s, d, count);
    __ align(CodeEntryAlignment);
    StubCodeMark mark(this, "StubRoutines", name);
    address start = __ pc();
    __ enter();

    if (entry != NULL) {
      *entry = __ pc();
      // caller can pass a 64-bit byte count here (from Unsafe.copyMemory)
      BLOCK_COMMENT("Entry:");
    }

    DecoratorSet decorators = IN_HEAP | IS_ARRAY | ARRAYCOPY_DISJOINT;
    if (dest_uninitialized) {
      decorators |= IS_DEST_UNINITIALIZED;
    }
    if (aligned) {
      decorators |= ARRAYCOPY_ALIGNED;
    }

    BarrierSetAssembler *bs = BarrierSet::barrier_set()->barrier_set_assembler();
    bs->arraycopy_prologue(_masm, decorators, is_oop, s, d, count, saved_reg);

    if (is_oop) {
      // save regs before copy_memory
      __ push_reg(RegSet::of(d, count), sp);
    }
    copy_memory(aligned, s, d, count, t0, checked_cast<int>(size));

    if (is_oop) {
      __ pop_reg(RegSet::of(d, count), sp);
      if (VerifyOops) {
        verify_oop_array(checked_cast<int>(size), d, count, t2);
      }
    }

    bs->arraycopy_epilogue(_masm, decorators, is_oop, d, count, t0, saved_reg);

    __ leave();
    __ mv(x10, zr); // return 0
    __ ret();
    return start;
  }

  // Arguments:
  //   aligned - true => Input and output aligned on a HeapWord == 8-byte boundary
  //             ignored
  //   is_oop  - true => oop array, so generate store check code
  //   name    - stub name string
  //
  // Inputs:
  //   c_rarg0   - source array address
  //   c_rarg1   - destination array address
  //   c_rarg2   - element count, treated as ssize_t, can be zero
  //
  // If 'from' and/or 'to' are aligned on 4-byte boundaries, we let
  // the hardware handle it.  The two dwords within qwords that span
  // cache line boundaries will still be loaded and stored atomically.
  //
  address generate_conjoint_copy(int size, bool aligned, bool is_oop, address nooverlap_target,
                                 address* entry, const char* name,
                                 bool dest_uninitialized = false) {
    const Register s = c_rarg0, d = c_rarg1, count = c_rarg2;
    RegSet saved_regs = RegSet::of(s, d, count);
    StubCodeMark mark(this, "StubRoutines", name);
    address start = __ pc();
    __ enter();

    if (entry != NULL) {
      *entry = __ pc();
      // caller can pass a 64-bit byte count here (from Unsafe.copyMemory)
      BLOCK_COMMENT("Entry:");
    }

    // use fwd copy when (d-s) above_equal (count*size)
    __ sub(t0, d, s);
    __ slli(t1, count, exact_log2(size));
    __ bgeu(t0, t1, nooverlap_target);

    DecoratorSet decorators = IN_HEAP | IS_ARRAY;
    if (dest_uninitialized) {
      decorators |= IS_DEST_UNINITIALIZED;
    }
    if (aligned) {
      decorators |= ARRAYCOPY_ALIGNED;
    }

    BarrierSetAssembler *bs = BarrierSet::barrier_set()->barrier_set_assembler();
    bs->arraycopy_prologue(_masm, decorators, is_oop, s, d, count, saved_regs);

    if (is_oop) {
      // save regs before copy_memory
      __ push_reg(RegSet::of(d, count), sp);
    }

    copy_memory(aligned, s, d, count, t0, -size);
    if (is_oop) {
      __ pop_reg(RegSet::of(d, count), sp);
      if (VerifyOops) {
        verify_oop_array(size, d, count, t2);
      }
    }
    bs->arraycopy_epilogue(_masm, decorators, is_oop, d, count, t0, saved_regs);
    __ leave();
    __ mv(x10, zr); // return 0
    __ ret();
    return start;
  }

  // Arguments:
  //   aligned - true => Input and output aligned on a HeapWord == 8-byte boundary
  //             ignored
  //   name    - stub name string
  //
  // Inputs:
  //   c_rarg0   - source array address
  //   c_rarg1   - destination array address
  //   c_rarg2   - element count, treated as ssize_t, can be zero
  //
  // If 'from' and/or 'to' are aligned on 4-, 2-, or 1-byte boundaries,
  // we let the hardware handle it.  The one to eight bytes within words,
  // dwords or qwords that span cache line boundaries will still be loaded
  // and stored atomically.
  //
  // Side Effects:
  //   disjoint_byte_copy_entry is set to the no-overlap entry point  //
  // If 'from' and/or 'to' are aligned on 4-, 2-, or 1-byte boundaries,
  // we let the hardware handle it.  The one to eight bytes within words,
  // dwords or qwords that span cache line boundaries will still be loaded
  // and stored atomically.
  //
  // Side Effects:
  //   disjoint_byte_copy_entry is set to the no-overlap entry point
  //   used by generate_conjoint_byte_copy().
  //
  address generate_disjoint_byte_copy(bool aligned, address* entry, const char* name) {
    const bool not_oop = false;
    return generate_disjoint_copy(sizeof (jbyte), aligned, not_oop, entry, name);
  }

  // Arguments:
  //   aligned - true => Input and output aligned on a HeapWord == 8-byte boundary
  //             ignored
  //   name    - stub name string
  //
  // Inputs:
  //   c_rarg0   - source array address
  //   c_rarg1   - destination array address
  //   c_rarg2   - element count, treated as ssize_t, can be zero
  //
  // If 'from' and/or 'to' are aligned on 4-, 2-, or 1-byte boundaries,
  // we let the hardware handle it.  The one to eight bytes within words,
  // dwords or qwords that span cache line boundaries will still be loaded
  // and stored atomically.
  //
  address generate_conjoint_byte_copy(bool aligned, address nooverlap_target,
                                      address* entry, const char* name) {
    const bool not_oop = false;
    return generate_conjoint_copy(sizeof (jbyte), aligned, not_oop, nooverlap_target, entry, name);
  }

  // Arguments:
  //   aligned - true => Input and output aligned on a HeapWord == 8-byte boundary
  //             ignored
  //   name    - stub name string
  //
  // Inputs:
  //   c_rarg0   - source array address
  //   c_rarg1   - destination array address
  //   c_rarg2   - element count, treated as ssize_t, can be zero
  //
  // If 'from' and/or 'to' are aligned on 4- or 2-byte boundaries, we
  // let the hardware handle it.  The two or four words within dwords
  // or qwords that span cache line boundaries will still be loaded
  // and stored atomically.
  //
  // Side Effects:
  //   disjoint_short_copy_entry is set to the no-overlap entry point
  //   used by generate_conjoint_short_copy().
  //
  address generate_disjoint_short_copy(bool aligned,
                                       address* entry, const char* name) {
    const bool not_oop = false;
    return generate_disjoint_copy(sizeof (jshort), aligned, not_oop, entry, name);
  }

  // Arguments:
  //   aligned - true => Input and output aligned on a HeapWord == 8-byte boundary
  //             ignored
  //   name    - stub name string
  //
  // Inputs:
  //   c_rarg0   - source array address
  //   c_rarg1   - destination array address
  //   c_rarg2   - element count, treated as ssize_t, can be zero
  //
  // If 'from' and/or 'to' are aligned on 4- or 2-byte boundaries, we
  // let the hardware handle it.  The two or four words within dwords
  // or qwords that span cache line boundaries will still be loaded
  // and stored atomically.
  //
  address generate_conjoint_short_copy(bool aligned, address nooverlap_target,
                                       address* entry, const char* name) {
    const bool not_oop = false;
    return generate_conjoint_copy(sizeof (jshort), aligned, not_oop, nooverlap_target, entry, name);
  }

  // Arguments:
  //   aligned - true => Input and output aligned on a HeapWord == 8-byte boundary
  //             ignored
  //   name    - stub name string
  //
  // Inputs:
  //   c_rarg0   - source array address
  //   c_rarg1   - destination array address
  //   c_rarg2   - element count, treated as ssize_t, can be zero
  //
  // If 'from' and/or 'to' are aligned on 4-byte boundaries, we let
  // the hardware handle it.  The two dwords within qwords that span
  // cache line boundaries will still be loaded and stored atomically.
  //
  // Side Effects:
  //   disjoint_int_copy_entry is set to the no-overlap entry point
  //   used by generate_conjoint_int_oop_copy().
  //
  address generate_disjoint_int_copy(bool aligned, address* entry,
                                     const char* name, bool dest_uninitialized = false) {
    const bool not_oop = false;
    return generate_disjoint_copy(sizeof (jint), aligned, not_oop, entry, name);
  }

  // Arguments:
  //   aligned - true => Input and output aligned on a HeapWord == 8-byte boundary
  //             ignored
  //   name    - stub name string
  //
  // Inputs:
  //   c_rarg0   - source array address
  //   c_rarg1   - destination array address
  //   c_rarg2   - element count, treated as ssize_t, can be zero
  //
  // If 'from' and/or 'to' are aligned on 4-byte boundaries, we let
  // the hardware handle it.  The two dwords within qwords that span
  // cache line boundaries will still be loaded and stored atomically.
  //
  address generate_conjoint_int_copy(bool aligned, address nooverlap_target,
                                     address* entry, const char* name,
                                     bool dest_uninitialized = false) {
    const bool not_oop = false;
    return generate_conjoint_copy(sizeof (jint), aligned, not_oop, nooverlap_target, entry, name);
  }


  // Arguments:
  //   aligned - true => Input and output aligned on a HeapWord boundary == 8 bytes
  //             ignored
  //   name    - stub name string
  //
  // Inputs:
  //   c_rarg0   - source array address
  //   c_rarg1   - destination array address
  //   c_rarg2   - element count, treated as size_t, can be zero
  //
  // Side Effects:
  //   disjoint_oop_copy_entry or disjoint_long_copy_entry is set to the
  //   no-overlap entry point used by generate_conjoint_long_oop_copy().
  //
  address generate_disjoint_long_copy(bool aligned, address* entry,
                                      const char* name, bool dest_uninitialized = false) {
    const bool not_oop = false;
    return generate_disjoint_copy(sizeof (jlong), aligned, not_oop, entry, name);
  }

  // Arguments:
  //   aligned - true => Input and output aligned on a HeapWord boundary == 8 bytes
  //             ignored
  //   name    - stub name string
  //
  // Inputs:
  //   c_rarg0   - source array address
  //   c_rarg1   - destination array address
  //   c_rarg2   - element count, treated as size_t, can be zero
  //
  address generate_conjoint_long_copy(bool aligned,
                                      address nooverlap_target, address* entry,
                                      const char* name, bool dest_uninitialized = false) {
    const bool not_oop = false;
    return generate_conjoint_copy(sizeof (jlong), aligned, not_oop, nooverlap_target, entry, name);
  }

  // Arguments:
  //   aligned - true => Input and output aligned on a HeapWord boundary == 8 bytes
  //             ignored
  //   name    - stub name string
  //
  // Inputs:
  //   c_rarg0   - source array address
  //   c_rarg1   - destination array address
  //   c_rarg2   - element count, treated as size_t, can be zero
  //
  // Side Effects:
  //   disjoint_oop_copy_entry or disjoint_long_copy_entry is set to the
  //   no-overlap entry point used by generate_conjoint_long_oop_copy().
  //
  address generate_disjoint_oop_copy(bool aligned, address* entry,
                                     const char* name, bool dest_uninitialized) {
    const bool is_oop = true;
    const int size = UseCompressedOops ? sizeof (jint) : sizeof (jlong);
    return generate_disjoint_copy(size, aligned, is_oop, entry, name, dest_uninitialized);
  }

  // Arguments:
  //   aligned - true => Input and output aligned on a HeapWord boundary == 8 bytes
  //             ignored
  //   name    - stub name string
  //
  // Inputs:
  //   c_rarg0   - source array address
  //   c_rarg1   - destination array address
  //   c_rarg2   - element count, treated as size_t, can be zero
  //
  address generate_conjoint_oop_copy(bool aligned,
                                     address nooverlap_target, address* entry,
                                     const char* name, bool dest_uninitialized) {
    const bool is_oop = true;
    const int size = UseCompressedOops ? sizeof (jint) : sizeof (jlong);
    return generate_conjoint_copy(size, aligned, is_oop, nooverlap_target, entry,
                                  name, dest_uninitialized);
  }

  // Helper for generating a dynamic type check.
  // Smashes t0, t1.
  void generate_type_check(Register sub_klass,
                           Register super_check_offset,
                           Register super_klass,
                           Label& L_success) {
    assert_different_registers(sub_klass, super_check_offset, super_klass);

    BLOCK_COMMENT("type_check:");

    Label L_miss;

    __ check_klass_subtype_fast_path(sub_klass, super_klass, noreg, &L_success, &L_miss, NULL, super_check_offset);
    __ check_klass_subtype_slow_path(sub_klass, super_klass, noreg, noreg, &L_success, NULL);

    // Fall through on failure!
    __ BIND(L_miss);
  }

  //
  //  Generate checkcasting array copy stub
  //
  //  Input:
  //    c_rarg0   - source array address
  //    c_rarg1   - destination array address
  //    c_rarg2   - element count, treated as ssize_t, can be zero
  //    c_rarg3   - size_t ckoff (super_check_offset)
  //    c_rarg4   - oop ckval (super_klass)
  //
  //  Output:
  //    x10 ==  0  -  success
  //    x10 == -1^K - failure, where K is partial transfer count
  //
  address generate_checkcast_copy(const char* name, address* entry,
                                  bool dest_uninitialized = false) {
    Label L_load_element, L_store_element, L_do_card_marks, L_done, L_done_pop;

    // Input registers (after setup_arg_regs)
    const Register from        = c_rarg0;   // source array address
    const Register to          = c_rarg1;   // destination array address
    const Register count       = c_rarg2;   // elementscount
    const Register ckoff       = c_rarg3;   // super_check_offset
    const Register ckval       = c_rarg4;   // super_klass

    RegSet wb_pre_saved_regs   = RegSet::range(c_rarg0, c_rarg4);
    RegSet wb_post_saved_regs  = RegSet::of(count);

    // Registers used as temps (x7, x9, x18 are save-on-entry)
    const Register count_save  = x19;       // orig elementscount
    const Register start_to    = x18;       // destination array start address
    const Register copied_oop  = x7;        // actual oop copied
    const Register r9_klass    = x9;        // oop._klass

    //---------------------------------------------------------------
    // Assembler stub will be used for this call to arraycopy
    // if the two arrays are subtypes of Object[] but the
    // destination array type is not equal to or a supertype
    // of the source type.  Each element must be separately
    // checked.

    assert_different_registers(from, to, count, ckoff, ckval, start_to,
                               copied_oop, r9_klass, count_save);

    __ align(CodeEntryAlignment);
    StubCodeMark mark(this, "StubRoutines", name);
    address start = __ pc();

    __ enter(); // required for proper stackwalking of RuntimeStub frame

    // Caller of this entry point must set up the argument registers
    if (entry != NULL) {
      *entry = __ pc();
      BLOCK_COMMENT("Entry:");
    }

    // Empty array:  Nothing to do
    __ beqz(count, L_done);

    __ push_reg(RegSet::of(x7, x9, x18, x19), sp);

#ifdef ASSERT
    BLOCK_COMMENT("assert consistent ckoff/ckval");
    // The ckoff and ckval must be mutually consistent,
    // even though caller generates both.
    { Label L;
      int sco_offset = in_bytes(Klass::super_check_offset_offset());
      __ lwu(start_to, Address(ckval, sco_offset));
      __ beq(ckoff, start_to, L);
      __ stop("super_check_offset inconsistent");
      __ bind(L);
    }
#endif //ASSERT

    DecoratorSet decorators = IN_HEAP | IS_ARRAY | ARRAYCOPY_CHECKCAST | ARRAYCOPY_DISJOINT;
    bool is_oop = true;
    if (dest_uninitialized) {
      decorators |= IS_DEST_UNINITIALIZED;
    }

    BarrierSetAssembler *bs = BarrierSet::barrier_set()->barrier_set_assembler();
    bs->arraycopy_prologue(_masm, decorators, is_oop, from, to, count, wb_pre_saved_regs);

    // save the original count
    __ mv(count_save, count);

    // Copy from low to high addresses
    __ mv(start_to, to);              // Save destination array start address
    __ j(L_load_element);

    // ======== begin loop ========
    // (Loop is rotated; its entry is L_load_element.)
    // Loop control:
    //   for count to 0 do
    //     copied_oop = load_heap_oop(from++)
    //     ... generate_type_check ...
    //     store_heap_oop(to++, copied_oop)
    //   end

    __ align(OptoLoopAlignment);

    __ BIND(L_store_element);
    __ store_heap_oop(Address(to, 0), copied_oop, noreg, noreg, AS_RAW);  // store the oop
    __ add(to, to, UseCompressedOops ? 4 : 8);
    __ sub(count, count, 1);
    __ beqz(count, L_do_card_marks);

    // ======== loop entry is here ========
    __ BIND(L_load_element);
    __ load_heap_oop(copied_oop, Address(from, 0), noreg, noreg, AS_RAW); // load the oop
    __ add(from, from, UseCompressedOops ? 4 : 8);
    __ beqz(copied_oop, L_store_element);

    __ load_klass(r9_klass, copied_oop);// query the object klass
    generate_type_check(r9_klass, ckoff, ckval, L_store_element);
    // ======== end loop ========

    // It was a real error; we must depend on the caller to finish the job.
    // Register count = remaining oops, count_orig = total oops.
    // Emit GC store barriers for the oops we have copied and report
    // their number to the caller.

    __ sub(count, count_save, count);     // K = partially copied oop count
    __ xori(count, count, -1);                   // report (-1^K) to caller
    __ beqz(count, L_done_pop);

    __ BIND(L_do_card_marks);
    bs->arraycopy_epilogue(_masm, decorators, is_oop, start_to, count_save, t0, wb_post_saved_regs);

    __ bind(L_done_pop);
    __ pop_reg(RegSet::of(x7, x9, x18, x19), sp);
    inc_counter_np(SharedRuntime::_checkcast_array_copy_ctr);

    __ bind(L_done);
    __ mv(x10, count);
    __ leave();
    __ ret();

    return start;
  }

  // Perform range checks on the proposed arraycopy.
  // Kills temp, but nothing else.
  // Also, clean the sign bits of src_pos and dst_pos.
  void arraycopy_range_checks(Register src,     // source array oop (c_rarg0)
                              Register src_pos, // source position (c_rarg1)
                              Register dst,     // destination array oo (c_rarg2)
                              Register dst_pos, // destination position (c_rarg3)
                              Register length,
                              Register temp,
                              Label& L_failed) {
    BLOCK_COMMENT("arraycopy_range_checks:");

    assert_different_registers(t0, temp);

    // if [src_pos + length > arrayOop(src)->length()] then FAIL
    __ lwu(t0, Address(src, arrayOopDesc::length_offset_in_bytes()));
    __ addw(temp, length, src_pos);
    __ bgtu(temp, t0, L_failed);

    // if [dst_pos + length > arrayOop(dst)->length()] then FAIL
    __ lwu(t0, Address(dst, arrayOopDesc::length_offset_in_bytes()));
    __ addw(temp, length, dst_pos);
    __ bgtu(temp, t0, L_failed);

    // Have to clean up high 32 bits of 'src_pos' and 'dst_pos'.
    __ clear_upper_bits(src_pos, 32);
    __ clear_upper_bits(dst_pos, 32);

    BLOCK_COMMENT("arraycopy_range_checks done");
  }

  //
  //  Generate 'unsafe' array copy stub
  //  Though just as safe as the other stubs, it takes an unscaled
  //  size_t argument instead of an element count.
  //
  //  Input:
  //    c_rarg0   - source array address
  //    c_rarg1   - destination array address
  //    c_rarg2   - byte count, treated as ssize_t, can be zero
  //
  // Examines the alignment of the operands and dispatches
  // to a long, int, short, or byte copy loop.
  //
  address generate_unsafe_copy(const char* name,
                               address byte_copy_entry,
                               address short_copy_entry,
                               address int_copy_entry,
                               address long_copy_entry) {
    assert_cond(byte_copy_entry != NULL && short_copy_entry != NULL &&
                int_copy_entry != NULL && long_copy_entry != NULL);
    Label L_long_aligned, L_int_aligned, L_short_aligned;
    const Register s = c_rarg0, d = c_rarg1, count = c_rarg2;

    __ align(CodeEntryAlignment);
    StubCodeMark mark(this, "StubRoutines", name);
    address start = __ pc();
    __ enter(); // required for proper stackwalking of RuntimeStub frame

    // bump this on entry, not on exit:
    inc_counter_np(SharedRuntime::_unsafe_array_copy_ctr);

    __ orr(t0, s, d);
    __ orr(t0, t0, count);

    __ andi(t0, t0, BytesPerLong - 1);
    __ beqz(t0, L_long_aligned);
    __ andi(t0, t0, BytesPerInt - 1);
    __ beqz(t0, L_int_aligned);
    __ andi(t0, t0, 1);
    __ beqz(t0, L_short_aligned);
    __ j(RuntimeAddress(byte_copy_entry));

    __ BIND(L_short_aligned);
    __ srli(count, count, LogBytesPerShort);  // size => short_count
    __ j(RuntimeAddress(short_copy_entry));
    __ BIND(L_int_aligned);
    __ srli(count, count, LogBytesPerInt);    // size => int_count
    __ j(RuntimeAddress(int_copy_entry));
    __ BIND(L_long_aligned);
    __ srli(count, count, LogBytesPerLong);   // size => long_count
    __ j(RuntimeAddress(long_copy_entry));

    return start;
  }

  //
  //  Generate generic array copy stubs
  //
  //  Input:
  //    c_rarg0    -  src oop
  //    c_rarg1    -  src_pos (32-bits)
  //    c_rarg2    -  dst oop
  //    c_rarg3    -  dst_pos (32-bits)
  //    c_rarg4    -  element count (32-bits)
  //
  //  Output:
  //    x10 ==  0  -  success
  //    x10 == -1^K - failure, where K is partial transfer count
  //
  address generate_generic_copy(const char* name,
                                address byte_copy_entry, address short_copy_entry,
                                address int_copy_entry, address oop_copy_entry,
                                address long_copy_entry, address checkcast_copy_entry) {
    assert_cond(byte_copy_entry != NULL && short_copy_entry != NULL &&
                int_copy_entry != NULL && oop_copy_entry != NULL &&
                long_copy_entry != NULL && checkcast_copy_entry != NULL);
    Label L_failed, L_failed_0, L_objArray;
    Label L_copy_bytes, L_copy_shorts, L_copy_ints, L_copy_longs;

    // Input registers
    const Register src        = c_rarg0;  // source array oop
    const Register src_pos    = c_rarg1;  // source position
    const Register dst        = c_rarg2;  // destination array oop
    const Register dst_pos    = c_rarg3;  // destination position
    const Register length     = c_rarg4;

    __ align(CodeEntryAlignment);

    StubCodeMark mark(this, "StubRoutines", name);

    // Registers used as temps
    const Register dst_klass = c_rarg5;

    address start = __ pc();

    __ enter(); // required for proper stackwalking of RuntimeStub frame

    // bump this on entry, not on exit:
    inc_counter_np(SharedRuntime::_generic_array_copy_ctr);

    //-----------------------------------------------------------------------
    // Assembler stub will be used for this call to arraycopy
    // if the following conditions are met:
    //
    // (1) src and dst must not be null.
    // (2) src_pos must not be negative.
    // (3) dst_pos must not be negative.
    // (4) length  must not be negative.
    // (5) src klass and dst klass should be the same and not NULL.
    // (6) src and dst should be arrays.
    // (7) src_pos + length must not exceed length of src.
    // (8) dst_pos + length must not exceed length of dst.
    //

    // if [src == NULL] then return -1
    __ beqz(src, L_failed);

    // if [src_pos < 0] then return -1
    // i.e. sign bit set
    __ andi(t0, src_pos, 1UL << 31);
    __ bnez(t0, L_failed);

    // if [dst == NULL] then return -1
    __ beqz(dst, L_failed);

    // if [dst_pos < 0] then return -1
    // i.e. sign bit set
    __ andi(t0, dst_pos, 1UL << 31);
    __ bnez(t0, L_failed);

    // registers used as temp
    const Register scratch_length    = x28; // elements count to copy
    const Register scratch_src_klass = x29; // array klass
    const Register lh                = x30; // layout helper

    // if [length < 0] then return -1
    __ addw(scratch_length, length, zr);    // length (elements count, 32-bits value)
    // i.e. sign bit set
    __ andi(t0, scratch_length, 1UL << 31);
    __ bnez(t0, L_failed);

    __ load_klass(scratch_src_klass, src);
#ifdef ASSERT
    {
      BLOCK_COMMENT("assert klasses not null {");
      Label L1, L2;
      __ bnez(scratch_src_klass, L2);   // it is broken if klass is NULL
      __ bind(L1);
      __ stop("broken null klass");
      __ bind(L2);
      __ load_klass(t0, dst);
      __ beqz(t0, L1);     // this would be broken also
      BLOCK_COMMENT("} assert klasses not null done");
    }
#endif

    // Load layout helper (32-bits)
    //
    //  |array_tag|     | header_size | element_type |     |log2_element_size|
    // 32        30    24            16              8     2                 0
    //
    //   array_tag: typeArray = 0x3, objArray = 0x2, non-array = 0x0
    //

    const int lh_offset = in_bytes(Klass::layout_helper_offset());

    // Handle objArrays completely differently...
    const jint objArray_lh = Klass::array_layout_helper(T_OBJECT);
    __ lw(lh, Address(scratch_src_klass, lh_offset));
    __ mvw(t0, objArray_lh);
    __ beq(lh, t0, L_objArray);

    // if [src->klass() != dst->klass()] then return -1
    __ load_klass(t1, dst);
    __ bne(t1, scratch_src_klass, L_failed);

    // if [src->is_Array() != NULL] then return -1
    // i.e. (lh >= 0)
    __ andi(t0, lh, 1UL << 31);
    __ beqz(t0, L_failed);

    // At this point, it is known to be a typeArray (array_tag 0x3).
#ifdef ASSERT
    {
      BLOCK_COMMENT("assert primitive array {");
      Label L;
      __ mvw(t1, Klass::_lh_array_tag_type_value << Klass::_lh_array_tag_shift);
      __ bge(lh, t1, L);
      __ stop("must be a primitive array");
      __ bind(L);
      BLOCK_COMMENT("} assert primitive array done");
    }
#endif

    arraycopy_range_checks(src, src_pos, dst, dst_pos, scratch_length,
                           t1, L_failed);

    // TypeArrayKlass
    //
    // src_addr = (src + array_header_in_bytes()) + (src_pos << log2elemsize)
    // dst_addr = (dst + array_header_in_bytes()) + (dst_pos << log2elemsize)
    //

    const Register t0_offset = t0;    // array offset
    const Register x22_elsize = lh;   // element size

    // Get array_header_in_bytes()
    int lh_header_size_width = exact_log2(Klass::_lh_header_size_mask + 1);
    int lh_header_size_msb = Klass::_lh_header_size_shift + lh_header_size_width;
    __ slli(t0_offset, lh, registerSize - lh_header_size_msb);          // left shift to remove 24 ~ 32;
    __ srli(t0_offset, t0_offset, registerSize - lh_header_size_width); // array_offset

    __ add(src, src, t0_offset);           // src array offset
    __ add(dst, dst, t0_offset);           // dst array offset
    BLOCK_COMMENT("choose copy loop based on element size");

    // next registers should be set before the jump to corresponding stub
    const Register from     = c_rarg0;  // source array address
    const Register to       = c_rarg1;  // destination array address
    const Register count    = c_rarg2;  // elements count

    // 'from', 'to', 'count' registers should be set in such order
    // since they are the same as 'src', 'src_pos', 'dst'.

    assert(Klass::_lh_log2_element_size_shift == 0, "fix this code");

    // The possible values of elsize are 0-3, i.e. exact_log2(element
    // size in bytes).  We do a simple bitwise binary search.
  __ BIND(L_copy_bytes);
    __ andi(t0, x22_elsize, 2);
    __ bnez(t0, L_copy_ints);
    __ andi(t0, x22_elsize, 1);
    __ bnez(t0, L_copy_shorts);
    __ add(from, src, src_pos); // src_addr
    __ add(to, dst, dst_pos); // dst_addr
    __ addw(count, scratch_length, zr); // length
    __ j(RuntimeAddress(byte_copy_entry));

  __ BIND(L_copy_shorts);
    __ slli(t0, src_pos, 1);
    __ add(from, src, t0); // src_addr
    __ slli(t0, dst_pos, 1);
    __ add(to, dst, t0); // dst_addr
    __ addw(count, scratch_length, zr); // length
    __ j(RuntimeAddress(short_copy_entry));

  __ BIND(L_copy_ints);
    __ andi(t0, x22_elsize, 1);
    __ bnez(t0, L_copy_longs);
    __ slli(t0, src_pos, 2);
    __ add(from, src, t0); // src_addr
    __ slli(t0, dst_pos, 2);
    __ add(to, dst, t0); // dst_addr
    __ addw(count, scratch_length, zr); // length
    __ j(RuntimeAddress(int_copy_entry));

  __ BIND(L_copy_longs);
#ifdef ASSERT
    {
      BLOCK_COMMENT("assert long copy {");
      Label L;
      __ andi(lh, lh, Klass::_lh_log2_element_size_mask); // lh -> x22_elsize
      __ addw(lh, lh, zr);
      __ mvw(t0, LogBytesPerLong);
      __ beq(x22_elsize, t0, L);
      __ stop("must be long copy, but elsize is wrong");
      __ bind(L);
      BLOCK_COMMENT("} assert long copy done");
    }
#endif
    __ slli(t0, src_pos, 3);
    __ add(from, src, t0); // src_addr
    __ slli(t0, dst_pos, 3);
    __ add(to, dst, t0); // dst_addr
    __ addw(count, scratch_length, zr); // length
    __ j(RuntimeAddress(long_copy_entry));

    // ObjArrayKlass
  __ BIND(L_objArray);
    // live at this point:  scratch_src_klass, scratch_length, src[_pos], dst[_pos]

    Label L_plain_copy, L_checkcast_copy;
    // test array classes for subtyping
    __ load_klass(t2, dst);
    __ bne(scratch_src_klass, t2, L_checkcast_copy); // usual case is exact equality

    // Identically typed arrays can be copied without element-wise checks.
    arraycopy_range_checks(src, src_pos, dst, dst_pos, scratch_length,
                           t1, L_failed);

    __ slli(t0, src_pos, LogBytesPerHeapOop);
    __ add(from, t0, src);
    __ add(from, from, arrayOopDesc::base_offset_in_bytes(T_OBJECT));
    __ slli(t0, dst_pos, LogBytesPerHeapOop);
    __ add(to, t0, dst);
    __ add(to, to, arrayOopDesc::base_offset_in_bytes(T_OBJECT));
    __ addw(count, scratch_length, zr); // length
  __ BIND(L_plain_copy);
    __ j(RuntimeAddress(oop_copy_entry));

  __ BIND(L_checkcast_copy);
    // live at this point:  scratch_src_klass, scratch_length, t2 (dst_klass)
    {
      // Before looking at dst.length, make sure dst is also an objArray.
      __ lwu(t0, Address(t2, lh_offset));
      __ mvw(t1, objArray_lh);
      __ bne(t0, t1, L_failed);

      // It is safe to examine both src.length and dst.length.
      arraycopy_range_checks(src, src_pos, dst, dst_pos, scratch_length,
                             t2, L_failed);

      __ load_klass(dst_klass, dst); // reload

      // Marshal the base address arguments now, freeing registers.
      __ slli(t0, src_pos, LogBytesPerHeapOop);
      __ add(from, t0, src);
      __ add(from, from, arrayOopDesc::base_offset_in_bytes(T_OBJECT));
      __ slli(t0, dst_pos, LogBytesPerHeapOop);
      __ add(to, t0, dst);
      __ add(to, to, arrayOopDesc::base_offset_in_bytes(T_OBJECT));
      __ addw(count, length, zr);           // length (reloaded)
      const Register sco_temp = c_rarg3;      // this register is free now
      assert_different_registers(from, to, count, sco_temp,
                                 dst_klass, scratch_src_klass);

      // Generate the type check.
      const int sco_offset = in_bytes(Klass::super_check_offset_offset());
      __ lwu(sco_temp, Address(dst_klass, sco_offset));

      // Smashes t0, t1
      generate_type_check(scratch_src_klass, sco_temp, dst_klass, L_plain_copy);

      // Fetch destination element klass from the ObjArrayKlass header.
      int ek_offset = in_bytes(ObjArrayKlass::element_klass_offset());
      __ ld(dst_klass, Address(dst_klass, ek_offset));
      __ lwu(sco_temp, Address(dst_klass, sco_offset));

      // the checkcast_copy loop needs two extra arguments:
      assert(c_rarg3 == sco_temp, "#3 already in place");
      // Set up arguments for checkcast_copy_entry.
      __ mv(c_rarg4, dst_klass);  // dst.klass.element_klass
      __ j(RuntimeAddress(checkcast_copy_entry));
    }

  __ BIND(L_failed);
    __ li(x10, -1);
    __ leave();   // required for proper stackwalking of RuntimeStub frame
    __ ret();

    return start;
  }

  //
  // Generate stub for array fill. If "aligned" is true, the
  // "to" address is assumed to be heapword aligned.
  //
  // Arguments for generated stub:
  //   to:    c_rarg0
  //   value: c_rarg1
  //   count: c_rarg2 treated as signed
  //
  address generate_fill(BasicType t, bool aligned, const char* name) {
    __ align(CodeEntryAlignment);
    StubCodeMark mark(this, "StubRoutines", name);
    address start = __ pc();

    BLOCK_COMMENT("Entry:");

    const Register to        = c_rarg0;  // source array address
    const Register value     = c_rarg1;  // value
    const Register count     = c_rarg2;  // elements count

    const Register bz_base   = x28;      // base for block_zero routine
    const Register cnt_words = x29;      // temp register
    const Register tmp_reg   = t1;

    __ enter();

    Label L_fill_elements, L_exit1;

    int shift = -1;
    switch (t) {
      case T_BYTE:
        shift = 0;

        // Zero extend value
        // 8 bit -> 16 bit
        __ andi(value, value, 0xff);
        __ mv(tmp_reg, value);
        __ slli(tmp_reg, tmp_reg, 8);
        __ orr(value, value, tmp_reg);

        // 16 bit -> 32 bit
        __ mv(tmp_reg, value);
        __ slli(tmp_reg, tmp_reg, 16);
        __ orr(value, value, tmp_reg);

        __ mv(tmp_reg, 8 >> shift); // Short arrays (< 8 bytes) fill by element
        __ bltu(count, tmp_reg, L_fill_elements);
        break;
      case T_SHORT:
        shift = 1;
        // Zero extend value
        // 16 bit -> 32 bit
        __ andi(value, value, 0xffff);
        __ mv(tmp_reg, value);
        __ slli(tmp_reg, tmp_reg, 16);
        __ orr(value, value, tmp_reg);

        // Short arrays (< 8 bytes) fill by element
        __ mv(tmp_reg, 8 >> shift);
        __ bltu(count, tmp_reg, L_fill_elements);
        break;
      case T_INT:
        shift = 2;

        // Short arrays (< 8 bytes) fill by element
        __ mv(tmp_reg, 8 >> shift);
        __ bltu(count, tmp_reg, L_fill_elements);
        break;
      default: ShouldNotReachHere();
    }

    // Align source address at 8 bytes address boundary.
    Label L_skip_align1, L_skip_align2, L_skip_align4;
    if (!aligned) {
      switch (t) {
        case T_BYTE:
          // One byte misalignment happens only for byte arrays.
          __ andi(t0, to, 1);
          __ beqz(t0, L_skip_align1);
          __ sb(value, Address(to, 0));
          __ addi(to, to, 1);
          __ addiw(count, count, -1);
          __ bind(L_skip_align1);
          // Fallthrough
        case T_SHORT:
          // Two bytes misalignment happens only for byte and short (char) arrays.
          __ andi(t0, to, 2);
          __ beqz(t0, L_skip_align2);
          __ sh(value, Address(to, 0));
          __ addi(to, to, 2);
          __ addiw(count, count, -(2 >> shift));
          __ bind(L_skip_align2);
          // Fallthrough
        case T_INT:
          // Align to 8 bytes, we know we are 4 byte aligned to start.
          __ andi(t0, to, 4);
          __ beqz(t0, L_skip_align4);
          __ sw(value, Address(to, 0));
          __ addi(to, to, 4);
          __ addiw(count, count, -(4 >> shift));
          __ bind(L_skip_align4);
          break;
        default: ShouldNotReachHere();
      }
    }

    //
    //  Fill large chunks
    //
    __ srliw(cnt_words, count, 3 - shift); // number of words

    // 32 bit -> 64 bit
    __ andi(value, value, 0xffffffff);
    __ mv(tmp_reg, value);
    __ slli(tmp_reg, tmp_reg, 32);
    __ orr(value, value, tmp_reg);

    __ slli(tmp_reg, cnt_words, 3 - shift);
    __ subw(count, count, tmp_reg);
    {
      __ fill_words(to, cnt_words, value);
    }

    // Remaining count is less than 8 bytes. Fill it by a single store.
    // Note that the total length is no less than 8 bytes.
    if (t == T_BYTE || t == T_SHORT) {
      __ beqz(count, L_exit1);
      __ slli(tmp_reg, count, shift);
      __ add(to, to, tmp_reg); // points to the end
      __ sd(value, Address(to, -8)); // overwrite some elements
      __ bind(L_exit1);
      __ leave();
      __ ret();
    }

    // Handle copies less than 8 bytes.
    Label L_fill_2, L_fill_4, L_exit2;
    __ bind(L_fill_elements);
    switch (t) {
      case T_BYTE:
        __ andi(t0, count, 1);
        __ beqz(t0, L_fill_2);
        __ sb(value, Address(to, 0));
        __ addi(to, to, 1);
        __ bind(L_fill_2);
        __ andi(t0, count, 2);
        __ beqz(t0, L_fill_4);
        __ sh(value, Address(to, 0));
        __ addi(to, to, 2);
        __ bind(L_fill_4);
        __ andi(t0, count, 4);
        __ beqz(t0, L_exit2);
        __ sw(value, Address(to, 0));
        break;
      case T_SHORT:
        __ andi(t0, count, 1);
        __ beqz(t0, L_fill_4);
        __ sh(value, Address(to, 0));
        __ addi(to, to, 2);
        __ bind(L_fill_4);
        __ andi(t0, count, 2);
        __ beqz(t0, L_exit2);
        __ sw(value, Address(to, 0));
        break;
      case T_INT:
        __ beqz(count, L_exit2);
        __ sw(value, Address(to, 0));
        break;
      default: ShouldNotReachHere();
    }
    __ bind(L_exit2);
    __ leave();
    __ ret();
    return start;
  }

  void generate_arraycopy_stubs() {
    address entry                     = NULL;
    address entry_jbyte_arraycopy     = NULL;
    address entry_jshort_arraycopy    = NULL;
    address entry_jint_arraycopy      = NULL;
    address entry_oop_arraycopy       = NULL;
    address entry_jlong_arraycopy     = NULL;
    address entry_checkcast_arraycopy = NULL;

    StubRoutines::riscv64::_zero_blocks = generate_zero_blocks();

    //*** jbyte
    // Always need aligned and unaligned versions
    StubRoutines::_jbyte_disjoint_arraycopy          = generate_disjoint_byte_copy(false, &entry,
                                                                                   "jbyte_disjoint_arraycopy");
    StubRoutines::_jbyte_arraycopy                   = generate_conjoint_byte_copy(false, entry,
                                                                                   &entry_jbyte_arraycopy,
                                                                                   "jbyte_arraycopy");
    StubRoutines::_arrayof_jbyte_disjoint_arraycopy  = generate_disjoint_byte_copy(true, &entry,
                                                                                   "arrayof_jbyte_disjoint_arraycopy");
    StubRoutines::_arrayof_jbyte_arraycopy           = generate_conjoint_byte_copy(true, entry, NULL,
                                                                                   "arrayof_jbyte_arraycopy");

    //*** jshort
    // Always need aligned and unaligned versions
    StubRoutines::_jshort_disjoint_arraycopy         = generate_disjoint_short_copy(false, &entry,
                                                                                    "jshort_disjoint_arraycopy");
    StubRoutines::_jshort_arraycopy                  = generate_conjoint_short_copy(false, entry,
                                                                                    &entry_jshort_arraycopy,
                                                                                    "jshort_arraycopy");
    StubRoutines::_arrayof_jshort_disjoint_arraycopy = generate_disjoint_short_copy(true, &entry,
                                                                                    "arrayof_jshort_disjoint_arraycopy");
    StubRoutines::_arrayof_jshort_arraycopy          = generate_conjoint_short_copy(true, entry, NULL,
                                                                                    "arrayof_jshort_arraycopy");

    //*** jint
    // Aligned versions
    StubRoutines::_arrayof_jint_disjoint_arraycopy   = generate_disjoint_int_copy(true, &entry,
                                                                                  "arrayof_jint_disjoint_arraycopy");
    StubRoutines::_arrayof_jint_arraycopy            = generate_conjoint_int_copy(true, entry, &entry_jint_arraycopy,
                                                                                  "arrayof_jint_arraycopy");
    // In 64 bit we need both aligned and unaligned versions of jint arraycopy.
    // entry_jint_arraycopy always points to the unaligned version
    StubRoutines::_jint_disjoint_arraycopy           = generate_disjoint_int_copy(false, &entry,
                                                                                  "jint_disjoint_arraycopy");
    StubRoutines::_jint_arraycopy                    = generate_conjoint_int_copy(false, entry,
                                                                                  &entry_jint_arraycopy,
                                                                                  "jint_arraycopy");

    //*** jlong
    // It is always aligned
    StubRoutines::_arrayof_jlong_disjoint_arraycopy  = generate_disjoint_long_copy(true, &entry,
                                                                                   "arrayof_jlong_disjoint_arraycopy");
    StubRoutines::_arrayof_jlong_arraycopy           = generate_conjoint_long_copy(true, entry, &entry_jlong_arraycopy,
                                                                                   "arrayof_jlong_arraycopy");
    StubRoutines::_jlong_disjoint_arraycopy          = StubRoutines::_arrayof_jlong_disjoint_arraycopy;
    StubRoutines::_jlong_arraycopy                   = StubRoutines::_arrayof_jlong_arraycopy;

    //*** oops
    {
      // With compressed oops we need unaligned versions; notice that
      // we overwrite entry_oop_arraycopy.
      bool aligned = !UseCompressedOops;

      StubRoutines::_arrayof_oop_disjoint_arraycopy
        = generate_disjoint_oop_copy(aligned, &entry, "arrayof_oop_disjoint_arraycopy",
                                     /*dest_uninitialized*/false);
      StubRoutines::_arrayof_oop_arraycopy
        = generate_conjoint_oop_copy(aligned, entry, &entry_oop_arraycopy, "arrayof_oop_arraycopy",
                                     /*dest_uninitialized*/false);
      // Aligned versions without pre-barriers
      StubRoutines::_arrayof_oop_disjoint_arraycopy_uninit
        = generate_disjoint_oop_copy(aligned, &entry, "arrayof_oop_disjoint_arraycopy_uninit",
                                     /*dest_uninitialized*/true);
      StubRoutines::_arrayof_oop_arraycopy_uninit
        = generate_conjoint_oop_copy(aligned, entry, NULL, "arrayof_oop_arraycopy_uninit",
                                     /*dest_uninitialized*/true);
    }

    StubRoutines::_oop_disjoint_arraycopy            = StubRoutines::_arrayof_oop_disjoint_arraycopy;
    StubRoutines::_oop_arraycopy                     = StubRoutines::_arrayof_oop_arraycopy;
    StubRoutines::_oop_disjoint_arraycopy_uninit     = StubRoutines::_arrayof_oop_disjoint_arraycopy_uninit;
    StubRoutines::_oop_arraycopy_uninit              = StubRoutines::_arrayof_oop_arraycopy_uninit;

    StubRoutines::_checkcast_arraycopy        = generate_checkcast_copy("checkcast_arraycopy", &entry_checkcast_arraycopy);
    StubRoutines::_checkcast_arraycopy_uninit = generate_checkcast_copy("checkcast_arraycopy_uninit", NULL,
                                                                        /*dest_uninitialized*/true);


    StubRoutines::_unsafe_arraycopy    = generate_unsafe_copy("unsafe_arraycopy",
                                                              entry_jbyte_arraycopy,
                                                              entry_jshort_arraycopy,
                                                              entry_jint_arraycopy,
                                                              entry_jlong_arraycopy);

    StubRoutines::_generic_arraycopy   = generate_generic_copy("generic_arraycopy",
                                                               entry_jbyte_arraycopy,
                                                               entry_jshort_arraycopy,
                                                               entry_jint_arraycopy,
                                                               entry_oop_arraycopy,
                                                               entry_jlong_arraycopy,
                                                               entry_checkcast_arraycopy);

    StubRoutines::_jbyte_fill = generate_fill(T_BYTE, false, "jbyte_fill");
    StubRoutines::_jshort_fill = generate_fill(T_SHORT, false, "jshort_fill");
    StubRoutines::_jint_fill = generate_fill(T_INT, false, "jint_fill");
    StubRoutines::_arrayof_jbyte_fill = generate_fill(T_BYTE, true, "arrayof_jbyte_fill");
    StubRoutines::_arrayof_jshort_fill = generate_fill(T_SHORT, true, "arrayof_jshort_fill");
    StubRoutines::_arrayof_jint_fill = generate_fill(T_INT, true, "arrayof_jint_fill");
  }

  // Safefetch stubs.
  void generate_safefetch(const char* name, int size, address* entry,
                          address* fault_pc, address* continuation_pc) {
    // safefetch signatures:
    //   int      SafeFetch32(int*      adr, int      errValue)
    //   intptr_t SafeFetchN (intptr_t* adr, intptr_t errValue)
    //
    // arguments:
    //   c_rarg0 = adr
    //   c_rarg1 = errValue
    //
    // result:
    //   PPC_RET  = *adr or errValue
    assert_cond(entry != NULL && fault_pc != NULL && continuation_pc != NULL);
    StubCodeMark mark(this, "StubRoutines", name);

    // Entry point, pc or function descriptor.
    *entry = __ pc();

    // Load *adr into c_rarg1, may fault.
    *fault_pc = __ pc();
    switch (size) {
      case 4:
        // int32_t
        __ lw(c_rarg1, Address(c_rarg0, 0));
        break;
      case 8:
        // int64_t
        __ ld(c_rarg1, Address(c_rarg0, 0));
        break;
      default:
        ShouldNotReachHere();
    }

    // return errValue or *adr
    *continuation_pc = __ pc();
    __ mv(x10, c_rarg1);
    __ ret();
  }

#ifdef COMPILER2
  // code for comparing 16 bytes of strings with same encoding
  void compare_string_16_bytes_same(Label& DIFF1, Label& DIFF2) {
    const Register result = x10, str1 = x11, cnt1 = x12, str2 = x13, tmp1 = x28, tmp2 = x29, tmp4 = x7, tmp5 = x31;
    __ ld(tmp5, Address(str1));
    __ addi(str1, str1, wordSize);
    __ xorr(tmp4, tmp1, tmp2);
    __ ld(cnt1, Address(str2));
    __ addi(str2, str2, wordSize);
    __ bnez(tmp4, DIFF1);
    __ ld(tmp1, Address(str1));
    __ addi(str1, str1, wordSize);
    __ xorr(tmp4, tmp5, cnt1);
    __ ld(tmp2, Address(str2));
    __ addi(str2, str2, wordSize);
    __ bnez(tmp4, DIFF2);
  }

  // code for comparing 8 characters of strings with Latin1 and Utf16 encoding
  void compare_string_8_x_LU(Register tmpL, Register tmpU, Register strL, Register strU, Label& DIFF) {
    const Register tmp = x30;
    __ ld(tmpL, Address(strL));
    __ addi(strL, strL, wordSize);
    __ ld(tmpU, Address(strU));
    __ addi(strU, strU, wordSize);
    __ inflate_lo32(tmp, tmpL);
    __ mv(t0, tmp);
    __ xorr(tmp, tmpU, t0);
    __ bnez(tmp, DIFF);

    __ ld(tmpU, Address(strU));
    __ addi(strU, strU, wordSize);
    __ inflate_hi32(tmp, tmpL);
    __ mv(t0, tmp);
    __ xorr(tmp, tmpU, t0);
    __ bnez(tmp, DIFF);
  }

  // x10  = result
  // x11  = str1
  // x12  = cnt1
  // x13  = str2
  // x14  = cnt2
  // x28  = tmp1
  // x29  = tmp2
  // x30  = tmp3
  address generate_compare_long_string_different_encoding(bool isLU) {
    __ align(CodeEntryAlignment);
    StubCodeMark mark(this, "StubRoutines", isLU ? "compare_long_string_different_encoding LU" : "compare_long_string_different_encoding UL");
    address entry = __ pc();
    Label SMALL_LOOP, TAIL, LOAD_LAST, DIFF, DONE, CALCULATE_DIFFERENCE;
    const Register result = x10, str1 = x11, cnt1 = x12, str2 = x13, cnt2 = x14,
                   tmp1 = x28, tmp2 = x29, tmp3 = x30, tmp4 = x7, tmp5 = x31;
    RegSet spilled_regs = RegSet::of(tmp4, tmp5);

    // cnt2 == amount of characters left to compare
    // Check already loaded first 4 symbols
    __ inflate_lo32(tmp3, isLU ? tmp1 : tmp2);
    __ mv(isLU ? tmp1 : tmp2, tmp3);
    __ addi(str1, str1, isLU ? wordSize / 2 : wordSize);
    __ addi(str2, str2, isLU ? wordSize : wordSize / 2);
    __ sub(cnt2, cnt2, wordSize / 2); // Already loaded 4 symbols.
    __ push_reg(spilled_regs, sp);

    __ xorr(tmp3, tmp1, tmp2);
    __ mv(tmp5, tmp2);
    __ bnez(tmp3, CALCULATE_DIFFERENCE);

    Register strU = isLU ? str2 : str1,
             strL = isLU ? str1 : str2,
             tmpU = isLU ? tmp5 : tmp1, // where to keep U for comparison
             tmpL = isLU ? tmp1 : tmp5; // where to keep L for comparison

    // make sure main loop is byte-aligned, we should load another 4 bytes from strL
    __ beqz(cnt2, DONE);  // no characters left
    __ lwu(tmpL, Address(strL));
    __ addi(strL, strL, wordSize / 2);
    __ ld(tmpU, Address(strU));
    __ addi(strU, strU, wordSize);
    __ inflate_lo32(tmp3, tmpL);
    __ mv(tmpL, tmp3);
    __ xorr(tmp3, tmpU, tmpL);
    __ bnez(tmp3, CALCULATE_DIFFERENCE);
    __ addi(cnt2, cnt2, -wordSize / 2);

    __ beqz(cnt2, DONE);  // no character left
    __ sub(cnt2, cnt2, wordSize * 2);
    __ bltz(cnt2, TAIL);
    __ bind(SMALL_LOOP); // smaller loop
      __ sub(cnt2, cnt2, wordSize * 2);
      compare_string_8_x_LU(tmpL, tmpU, strL, strU, DIFF);
      compare_string_8_x_LU(tmpL, tmpU, strL, strU, DIFF);
      __ bgez(cnt2, SMALL_LOOP);
      __ addi(t0, cnt2, wordSize * 2);
      __ beqz(t0, DONE);
    __ bind(TAIL);  // 1..15 characters left
      if (AvoidUnalignedAccesses) {
        // Aligned access. Load bytes from byte-aligned address,
        // which may contain invalid bytes in last load.
        // Invalid bytes should be removed before comparison.
        Label LOAD_LAST, WORD_CMP;
        __ addi(t0, cnt2, wordSize);
        __ bgtz(t0, LOAD_LAST);
        // remaining characters is greater than or equals to 8, we can do one compare_string_8_x_LU
        compare_string_8_x_LU(tmpL, tmpU, strL, strU, DIFF);
        __ addi(cnt2, cnt2, wordSize);
        __ beqz(cnt2, DONE);  // no character left
        __ bind(LOAD_LAST);   // 1..7 characters left
        __ lwu(tmpL, Address(strL));
        __ addi(strL, strL, wordSize / 2);
        __ ld(tmpU, Address(strU));
        __ addi(strU, strU, wordSize);
        __ inflate_lo32(tmp3, tmpL);
        __ mv(tmpL, tmp3);
        __ addi(t0, cnt2, wordSize / 2);
        __ blez(t0, WORD_CMP);
        __ slli(t0, t0, 1); // now in bytes
        __ slli(t0, t0, LogBitsPerByte);
        __ sll(tmpL, tmpL, t0);
        __ sll(tmpU, tmpU, t0);
        // remaining characters is greater than or equals to 4, we can do one full 4-byte comparison
        __ bind(WORD_CMP);
        __ xorr(tmp3, tmpU, tmpL);
        __ bnez(tmp3, CALCULATE_DIFFERENCE);
        __ addi(cnt2, cnt2, wordSize / 2);
        __ bltz(cnt2, LOAD_LAST); // 1..3 characters left
        __ j(DONE); // no character left
      } else {
        // Unaligned accesses. Load from non-byte aligned address.
        __ slli(t0, cnt2, 1);     // now in bytes
        __ add(strU, strU, t0);   // Address of last 8 bytes in UTF-16 string
        __ add(strL, strL, cnt2); // Address of last 16 bytes in Latin1 string
        // last 16 characters
        compare_string_8_x_LU(tmpL, tmpU, strL, strU, DIFF);
        compare_string_8_x_LU(tmpL, tmpU, strL, strU, DIFF);
        __ j(DONE);
      }
    __ bind(DIFF);
      __ mv(tmpL, t0);
      // Find the first different characters in the longwords and
      // compute their difference.
    __ bind(CALCULATE_DIFFERENCE);
      __ ctzc_bit(tmp4, tmp3);
      __ srl(tmp1, tmp1, tmp4);
      __ srl(tmp5, tmp5, tmp4);
      __ andi(tmp1, tmp1, 0xFFFF);
      __ andi(tmp5, tmp5, 0xFFFF);
      __ sub(result, tmp1, tmp5);
    __ bind(DONE);
      __ pop_reg(spilled_regs, sp);
      __ ret();
    return entry;
  }

  // x10  = result
  // x11  = str1
  // x12  = cnt1
  // x13  = str2
  // x14  = cnt2
  // x28  = tmp1
  // x29  = tmp2
  // x30  = tmp3
  // x31  = tmp4
  address generate_compare_long_string_same_encoding(bool isLL) {
    __ align(CodeEntryAlignment);
    StubCodeMark mark(this, "StubRoutines", isLL ?
                      "compare_long_string_same_encoding LL" : "compare_long_string_same_encoding UU");
    address entry = __ pc();
    Label SMALL_LOOP, CHECK_LAST, DIFF2, TAIL,
          LENGTH_DIFF, DIFF, LAST_CHECK_AND_LENGTH_DIFF;
    const Register result = x10, str1 = x11, cnt1 = x12, str2 = x13, cnt2 = x14,
                   tmp1 = x28, tmp2 = x29, tmp3 = x30, tmp4 = x7, tmp5 = x31;
    RegSet spilled_regs = RegSet::of(tmp4, tmp5);

    // cnt1/cnt2 contains amount of characters to compare. cnt1 can be re-used
    // update cnt2 counter with already loaded 8 bytes
    __ sub(cnt2, cnt2, wordSize / (isLL ? 1 : 2));
    // update pointers, because of previous read
    __ add(str1, str1, wordSize);
    __ add(str2, str2, wordSize);
    // less than 16 bytes left?
    __ sub(cnt2, cnt2, isLL ? 2 * wordSize : wordSize);
    __ push_reg(spilled_regs, sp);
    __ bltz(cnt2, TAIL);
    __ bind(SMALL_LOOP);
      compare_string_16_bytes_same(DIFF, DIFF2);
      __ sub(cnt2, cnt2, isLL ? 2 * wordSize : wordSize);
      __ bgez(cnt2, SMALL_LOOP);
    __ bind(TAIL);
      __ addi(cnt2, cnt2, isLL ? 2 * wordSize : wordSize);
      __ beqz(cnt2, LAST_CHECK_AND_LENGTH_DIFF);
      __ sub(cnt2, cnt2, isLL ? wordSize : wordSize / 2);
      __ blez(cnt2, CHECK_LAST);
      __ xorr(tmp4, tmp1, tmp2);
      __ bnez(tmp4, DIFF);
      __ ld(tmp1, Address(str1));
      __ addi(str1, str1, wordSize);
      __ ld(tmp2, Address(str2));
      __ addi(str2, str2, wordSize);
      __ sub(cnt2, cnt2, isLL ? wordSize : wordSize / 2);
    __ bind(CHECK_LAST);
      if (!isLL) {
        __ add(cnt2, cnt2, cnt2); // now in bytes
      }
      __ xorr(tmp4, tmp1, tmp2);
      __ bnez(tmp4, DIFF);
      if (AvoidUnalignedAccesses) {
        // Aligned access. Load bytes from byte-aligned address,
        // which may contain invalid bytes in last load.
        // Invalid bytes should be removed before comparison.
        __ ld(tmp5, Address(str1));
        __ ld(cnt1, Address(str2));
        __ neg(cnt2, cnt2);
        __ slli(cnt2, cnt2, LogBitsPerByte);
        __ sll(tmp5, tmp5, cnt2);
        __ sll(cnt1, cnt1, cnt2);
      } else {
        // Unaligned access. Load from non-byte aligned address.
        __ add(str1, str1, cnt2);
        __ ld(tmp5, Address(str1));
        __ add(str2, str2, cnt2);
        __ ld(cnt1, Address(str2));
      }

      __ xorr(tmp4, tmp5, cnt1);
      __ beqz(tmp4, LENGTH_DIFF);
      // Find the first different characters in the longwords and
      // compute their difference.
    __ bind(DIFF2);
      __ ctzc_bit(tmp3, tmp4, isLL); // count zero from lsb to msb
      __ srl(tmp5, tmp5, tmp3);
      __ srl(cnt1, cnt1, tmp3);
      if (isLL) {
        __ andi(tmp5, tmp5, 0xFF);
        __ andi(cnt1, cnt1, 0xFF);
      } else {
        __ andi(tmp5, tmp5, 0xFFFF);
        __ andi(cnt1, cnt1, 0xFFFF);
      }
      __ sub(result, tmp5, cnt1);
      __ j(LENGTH_DIFF);
    __ bind(DIFF);
      __ ctzc_bit(tmp3, tmp4, isLL); // count zero from lsb to msb
      __ srl(tmp1, tmp1, tmp3);
      __ srl(tmp2, tmp2, tmp3);
      if (isLL) {
        __ andi(tmp1, tmp1, 0xFF);
        __ andi(tmp2, tmp2, 0xFF);
      } else {
        __ andi(tmp1, tmp1, 0xFFFF);
        __ andi(tmp2, tmp2, 0xFFFF);
      }
      __ sub(result, tmp1, tmp2);
      __ j(LENGTH_DIFF);
    __ bind(LAST_CHECK_AND_LENGTH_DIFF);
      __ xorr(tmp4, tmp1, tmp2);
      __ bnez(tmp4, DIFF);
    __ bind(LENGTH_DIFF);
      __ pop_reg(spilled_regs, sp);
      __ ret();
    return entry;
  }

  void generate_compare_long_strings() {
    StubRoutines::riscv64::_compare_long_string_LL = generate_compare_long_string_same_encoding(true);
    StubRoutines::riscv64::_compare_long_string_UU = generate_compare_long_string_same_encoding(false);
    StubRoutines::riscv64::_compare_long_string_LU = generate_compare_long_string_different_encoding(true);
    StubRoutines::riscv64::_compare_long_string_UL = generate_compare_long_string_different_encoding(false);
  }

  // x10 result
  // x11 src
  // x12 src count
  // x13 pattern
  // x14 pattern count
  address generate_string_indexof_linear(bool needle_isL, bool haystack_isL)
  {
    const char* stubName = needle_isL
           ? (haystack_isL ? "indexof_linear_ll" : "indexof_linear_ul")
           : "indexof_linear_uu";
    __ align(CodeEntryAlignment);
    StubCodeMark mark(this, "StubRoutines", stubName);
    address entry = __ pc();

    int needle_chr_size = needle_isL ? 1 : 2;
    int haystack_chr_size = haystack_isL ? 1 : 2;
    int needle_chr_shift = needle_isL ? 0 : 1;
    int haystack_chr_shift = haystack_isL ? 0 : 1;
    bool isL = needle_isL && haystack_isL;
    // parameters
    Register result = x10, haystack = x11, haystack_len = x12, needle = x13, needle_len = x14;
    // temporary registers
    Register mask1 = x20, match_mask = x21, first = x22, trailing_zero = x23, mask2 = x24, tmp = x25;
    // redefinitions
    Register ch1 = x28, ch2 = x29;
    RegSet spilled_regs = RegSet::range(x20, x25) + RegSet::range(x28, x29);

    __ push_reg(spilled_regs, sp);

    Label L_LOOP, L_LOOP_PROCEED, L_SMALL, L_HAS_ZERO,
          L_HAS_ZERO_LOOP, L_CMP_LOOP, L_CMP_LOOP_NOMATCH, L_SMALL_PROCEED,
          L_SMALL_HAS_ZERO_LOOP, L_SMALL_CMP_LOOP_NOMATCH, L_SMALL_CMP_LOOP,
          L_POST_LOOP, L_CMP_LOOP_LAST_CMP, L_HAS_ZERO_LOOP_NOMATCH,
          L_SMALL_CMP_LOOP_LAST_CMP, L_SMALL_CMP_LOOP_LAST_CMP2,
          L_CMP_LOOP_LAST_CMP2, DONE, NOMATCH;

    __ ld(ch1, Address(needle));
    __ ld(ch2, Address(haystack));
    // src.length - pattern.length
    __ sub(haystack_len, haystack_len, needle_len);

    // first is needle[0]
    __ andi(first, ch1, needle_isL ? 0xFF : 0xFFFF, first);
    __ mv(mask1, haystack_isL ? 0x0101010101010101 : 0x0001000100010001);
    __ mul(first, first, mask1);
    __ mv(mask2, haystack_isL ? 0x7f7f7f7f7f7f7f7f : 0x7fff7fff7fff7fff);
    if (needle_isL != haystack_isL) {
      __ mv(tmp, ch1);
    }
    __ sub(haystack_len, haystack_len, wordSize / haystack_chr_size - 1);
    __ blez(haystack_len, L_SMALL);

    if (needle_isL != haystack_isL) {
      __ inflate_lo32(ch1, tmp, match_mask, trailing_zero);
    }
    // xorr, sub, orr, notr, andr
    // compare and set match_mask[i] with 0x80/0x8000 (Latin1/UTF16) if ch2[i] == first[i]
    // eg:
    // first:        aa aa aa aa aa aa aa aa
    // ch2:          aa aa li nx jd ka aa aa
    // match_mask:   80 80 00 00 00 00 80 80
    __ compute_match_mask(ch2, first, match_mask, mask1, mask2);

    // search first char of needle, if success, goto L_HAS_ZERO;
    __ bnez(match_mask, L_HAS_ZERO);
    __ sub(haystack_len, haystack_len, wordSize / haystack_chr_size);
    __ add(result, result, wordSize / haystack_chr_size);
    __ add(haystack, haystack, wordSize);
    __ bltz(haystack_len, L_POST_LOOP);

    __ bind(L_LOOP);
    __ ld(ch2, Address(haystack));
    __ compute_match_mask(ch2, first, match_mask, mask1, mask2);
    __ bnez(match_mask, L_HAS_ZERO);

    __ bind(L_LOOP_PROCEED);
    __ sub(haystack_len, haystack_len, wordSize / haystack_chr_size);
    __ add(haystack, haystack, wordSize);
    __ add(result, result, wordSize / haystack_chr_size);
    __ bgez(haystack_len, L_LOOP);

    __ bind(L_POST_LOOP);
    __ mv(ch2, -wordSize / haystack_chr_size);
    __ ble(haystack_len, ch2, NOMATCH); // no extra characters to check
    __ ld(ch2, Address(haystack));
    __ slli(haystack_len, haystack_len, LogBitsPerByte + haystack_chr_shift);
    __ neg(haystack_len, haystack_len);
    __ xorr(ch2, first, ch2);
    __ sub(match_mask, ch2, mask1);
    __ orr(ch2, ch2, mask2);
    __ mv(trailing_zero, -1); // all bits set
    __ j(L_SMALL_PROCEED);

    __ align(OptoLoopAlignment);
    __ bind(L_SMALL);
    __ slli(haystack_len, haystack_len, LogBitsPerByte + haystack_chr_shift);
    __ neg(haystack_len, haystack_len);
    if (needle_isL != haystack_isL) {
      __ inflate_lo32(ch1, tmp, match_mask, trailing_zero);
    }
    __ xorr(ch2, first, ch2);
    __ sub(match_mask, ch2, mask1);
    __ orr(ch2, ch2, mask2);
    __ mv(trailing_zero, -1); // all bits set

    __ bind(L_SMALL_PROCEED);
    __ srl(trailing_zero, trailing_zero, haystack_len); // mask. zeroes on useless bits.
    __ notr(ch2, ch2);
    __ andr(match_mask, match_mask, ch2);
    __ andr(match_mask, match_mask, trailing_zero); // clear useless bits and check
    __ beqz(match_mask, NOMATCH);

    __ bind(L_SMALL_HAS_ZERO_LOOP);
    __ ctzc_bit(trailing_zero, match_mask, haystack_isL, ch2, tmp); // count trailing zeros
    __ addi(trailing_zero, trailing_zero, haystack_isL ? 7 : 15);
    __ mv(ch2, wordSize / haystack_chr_size);
    __ ble(needle_len, ch2, L_SMALL_CMP_LOOP_LAST_CMP2);
    __ compute_index(haystack, trailing_zero, match_mask, result, ch2, tmp, haystack_isL);
    __ mv(trailing_zero, wordSize / haystack_chr_size);
    __ bne(ch1, ch2, L_SMALL_CMP_LOOP_NOMATCH);

    __ bind(L_SMALL_CMP_LOOP);
    __ slli(first, trailing_zero, needle_chr_shift);
    __ add(first, needle, first);
    __ slli(ch2, trailing_zero, haystack_chr_shift);
    __ add(ch2, haystack, ch2);
    needle_isL ? __ lbu(first, Address(first)) : __ lhu(first, Address(first));
    haystack_isL ? __ lbu(ch2, Address(ch2)) : __ lhu(ch2, Address(ch2));
    __ add(trailing_zero, trailing_zero, 1);
    __ bge(trailing_zero, needle_len, L_SMALL_CMP_LOOP_LAST_CMP);
    __ beq(first, ch2, L_SMALL_CMP_LOOP);

    __ bind(L_SMALL_CMP_LOOP_NOMATCH);
    __ beqz(match_mask, NOMATCH);
    __ ctzc_bit(trailing_zero, match_mask, haystack_isL, tmp, ch2);
    __ addi(trailing_zero, trailing_zero, haystack_isL ? 7 : 15);
    __ add(result, result, 1);
    __ add(haystack, haystack, haystack_chr_size);
    __ j(L_SMALL_HAS_ZERO_LOOP);

    __ align(OptoLoopAlignment);
    __ bind(L_SMALL_CMP_LOOP_LAST_CMP);
    __ bne(first, ch2, L_SMALL_CMP_LOOP_NOMATCH);
    __ j(DONE);

    __ align(OptoLoopAlignment);
    __ bind(L_SMALL_CMP_LOOP_LAST_CMP2);
    __ compute_index(haystack, trailing_zero, match_mask, result, ch2, tmp, haystack_isL);
    __ bne(ch1, ch2, L_SMALL_CMP_LOOP_NOMATCH);
    __ j(DONE);

    __ align(OptoLoopAlignment);
    __ bind(L_HAS_ZERO);
    __ ctzc_bit(trailing_zero, match_mask, haystack_isL, tmp, ch2);
    __ addi(trailing_zero, trailing_zero, haystack_isL ? 7 : 15);
    __ slli(needle_len, needle_len, BitsPerByte * wordSize / 2);
    __ orr(haystack_len, haystack_len, needle_len); // restore needle_len(32bits)
    __ sub(result, result, 1); // array index from 0, so result -= 1

    __ bind(L_HAS_ZERO_LOOP);
    __ mv(needle_len, wordSize / haystack_chr_size);
    __ srli(ch2, haystack_len, BitsPerByte * wordSize / 2);
    __ bge(needle_len, ch2, L_CMP_LOOP_LAST_CMP2);
    // load next 8 bytes from haystack, and increase result index
    __ compute_index(haystack, trailing_zero, match_mask, result, ch2, tmp, haystack_isL);
    __ add(result, result, 1);
    __ mv(trailing_zero, wordSize / haystack_chr_size);
    __ bne(ch1, ch2, L_CMP_LOOP_NOMATCH);

    // compare one char
    __ bind(L_CMP_LOOP);
    __ slli(needle_len, trailing_zero, needle_chr_shift);
    __ add(needle_len, needle, needle_len);
    needle_isL ? __ lbu(needle_len, Address(needle_len)) : __ lhu(needle_len, Address(needle_len));
    __ slli(ch2, trailing_zero, haystack_chr_shift);
    __ add(ch2, haystack, ch2);
    haystack_isL ? __ lbu(ch2, Address(ch2)) : __ lhu(ch2, Address(ch2));
    __ add(trailing_zero, trailing_zero, 1); // next char index
    __ srli(tmp, haystack_len, BitsPerByte * wordSize / 2);
    __ bge(trailing_zero, tmp, L_CMP_LOOP_LAST_CMP);
    __ beq(needle_len, ch2, L_CMP_LOOP);

    __ bind(L_CMP_LOOP_NOMATCH);
    __ beqz(match_mask, L_HAS_ZERO_LOOP_NOMATCH);
    __ ctzc_bit(trailing_zero, match_mask, haystack_isL, needle_len, ch2); // find next "first" char index
    __ addi(trailing_zero, trailing_zero, haystack_isL ? 7 : 15);
    __ add(haystack, haystack, haystack_chr_size);
    __ j(L_HAS_ZERO_LOOP);

    __ align(OptoLoopAlignment);
    __ bind(L_CMP_LOOP_LAST_CMP);
    __ bne(needle_len, ch2, L_CMP_LOOP_NOMATCH);
    __ j(DONE);

    __ align(OptoLoopAlignment);
    __ bind(L_CMP_LOOP_LAST_CMP2);
    __ compute_index(haystack, trailing_zero, match_mask, result, ch2, tmp, haystack_isL);
    __ add(result, result, 1);
    __ bne(ch1, ch2, L_CMP_LOOP_NOMATCH);
    __ j(DONE);

    __ align(OptoLoopAlignment);
    __ bind(L_HAS_ZERO_LOOP_NOMATCH);
    // 1) Restore "result" index. Index was wordSize/str2_chr_size * N until
    // L_HAS_ZERO block. Byte octet was analyzed in L_HAS_ZERO_LOOP,
    // so, result was increased at max by wordSize/str2_chr_size - 1, so,
    // respective high bit wasn't changed. L_LOOP_PROCEED will increase
    // result by analyzed characters value, so, we can just reset lower bits
    // in result here. Clear 2 lower bits for UU/UL and 3 bits for LL
    // 2) restore needle_len and haystack_len values from "compressed" haystack_len
    // 3) advance haystack value to represent next haystack octet. result & 7/3 is
    // index of last analyzed substring inside current octet. So, haystack in at
    // respective start address. We need to advance it to next octet
    __ andi(match_mask, result, wordSize / haystack_chr_size - 1);
    __ srli(needle_len, haystack_len, BitsPerByte * wordSize / 2);
    __ andi(result, result, haystack_isL ? -8 : -4);
    __ slli(tmp, match_mask, haystack_chr_shift);
    __ sub(haystack, haystack, tmp);
    __ addw(haystack_len, haystack_len, zr);
    __ j(L_LOOP_PROCEED);

    __ align(OptoLoopAlignment);
    __ bind(NOMATCH);
    __ mv(result, -1);

    __ bind(DONE);
    __ pop_reg(spilled_regs, sp);
    __ ret();
    return entry;
  }

  void generate_string_indexof_stubs()
  {
    StubRoutines::riscv64::_string_indexof_linear_ll = generate_string_indexof_linear(true, true);
    StubRoutines::riscv64::_string_indexof_linear_uu = generate_string_indexof_linear(false, false);
    StubRoutines::riscv64::_string_indexof_linear_ul = generate_string_indexof_linear(true, false);
  }

  address generate_mulAdd()
  {
    __ align(CodeEntryAlignment);
    StubCodeMark mark(this, "StubRoutines", "mulAdd");

    address start = __ pc();

    const Register out     = x10;
    const Register in      = x11;
    const Register offset  = x12;
    const Register len     = x13;
    const Register k       = x14;
    const Register tmp1    = x28;
    const Register tmp2    = x29;

    BLOCK_COMMENT("Entry:");
    __ enter();
    __ mul_add(out, in, offset, len, k, tmp1, tmp2);
    __ leave();
    __ ret();

    return start;
  }

  /**
   *  Arguments:
   *
   *  Input:
   *    c_rarg0   - x address
   *    c_rarg1   - x length
   *    c_rarg2   - y address
   *    c_rarg3   - y lenth
   *    c_rarg4   - z address
   *    c_rarg5   - z length
   */
  address generate_multiplyToLen()
  {
    __ align(CodeEntryAlignment);
    StubCodeMark mark(this, "StubRoutines", "multiplyToLen");
    address start = __ pc();

    const Register x     = x10;
    const Register xlen  = x11;
    const Register y     = x12;
    const Register ylen  = x13;
    const Register z     = x14;
    const Register zlen  = x15;

    const Register tmp1  = x16;
    const Register tmp2  = x17;
    const Register tmp3  = x7;
    const Register tmp4  = x28;
    const Register tmp5  = x29;
    const Register tmp6  = x30;
    const Register tmp7  = x31;

    RegSet spilled_regs = RegSet::of(tmp1, tmp2);
    BLOCK_COMMENT("Entry:");
    __ enter(); // required for proper stackwalking of RuntimeStub frame
    __ push_reg(spilled_regs, sp);
    __ multiply_to_len(x, xlen, y, ylen, z, zlen, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7);
    __ pop_reg(spilled_regs, sp);
    __ leave(); // required for proper stackwalking of RuntimeStub frame
    __ ret();

    return start;
  }

  address generate_squareToLen()
  {
    // squareToLen algorithm for sizes 1..127 described in java code works
    // faster than multiply_to_len on some CPUs and slower on others, but
    // multiply_to_len shows a bit better overall results
    __ align(CodeEntryAlignment);
    StubCodeMark mark(this, "StubRoutines", "squareToLen");
    address start = __ pc();

    const Register x     = x10;
    const Register xlen  = x11;
    const Register z     = x12;
    const Register zlen  = x13;
    const Register y     = x14; // == x
    const Register ylen  = x15; // == xlen

    const Register tmp1  = x16;
    const Register tmp2  = x17;
    const Register tmp3  = x7;
    const Register tmp4  = x28;
    const Register tmp5  = x29;
    const Register tmp6  = x30;
    const Register tmp7  = x31;

    RegSet spilled_regs = RegSet::of(y, tmp2);
    BLOCK_COMMENT("Entry:");
    __ enter();
    __ push_reg(spilled_regs, sp);
    __ mv(y, x);
    __ mv(ylen, xlen);
    __ multiply_to_len(x, xlen, y, ylen, z, zlen, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7);
    __ pop_reg(spilled_regs, sp);
    __ leave();
    __ ret();

    return start;
  }
#endif // COMPILER2

  // Continuation point for throwing of implicit exceptions that are
  // not handled in the current activation. Fabricates an exception
  // oop and initiates normal exception dispatching in this
  // frame. Since we need to preserve callee-saved values (currently
  // only for C2, but done for C1 as well) we need a callee-saved oop
  // map and therefore have to make these stubs into RuntimeStubs
  // rather than BufferBlobs.  If the compiler needs all registers to
  // be preserved between the fault point and the exception handler
  // then it must assume responsibility for that in
  // AbstractCompiler::continuation_for_implicit_null_exception or
  // continuation_for_implicit_division_by_zero_exception. All other
  // implicit exceptions (e.g., NullPointerException or
  // AbstractMethodError on entry) are either at call sites or
  // otherwise assume that stack unwinding will be initiated, so
  // caller saved registers were assumed volatile in the compiler.

#undef __
#define __ masm->

  address generate_throw_exception(const char* name,
                                   address runtime_entry,
                                   Register arg1 = noreg,
                                   Register arg2 = noreg) {
    // Information about frame layout at time of blocking runtime call.
    // Note that we only have to preserve callee-saved registers since
    // the compilers are responsible for supplying a continuation point
    // if they expect all registers to be preserved.
    // n.b. riscv64 asserts that frame::arg_reg_save_area_bytes == 0
    assert_cond(runtime_entry != NULL);
    enum layout {
      fp_off = 0,
      fp_off2,
      return_off,
      return_off2,
      framesize // inclusive of return address
    };

    const int insts_size = 512;
    const int locs_size  = 64;

    CodeBuffer code(name, insts_size, locs_size);
    OopMapSet* oop_maps  = new OopMapSet();
    MacroAssembler* masm = new MacroAssembler(&code);
    assert_cond(oop_maps != NULL && masm != NULL);

    address start = __ pc();

    // This is an inlined and slightly modified version of call_VM
    // which has the ability to fetch the return PC out of
    // thread-local storage and also sets up last_Java_sp slightly
    // differently than the real call_VM

    __ enter(); // Save FP and RA before call

    assert(is_even(framesize / 2), "sp not 16-byte aligned");

    // ra and fp are already in place
    __ addi(sp, fp, 0 - ((unsigned)framesize << LogBytesPerInt)); // prolog

    int frame_complete = __ pc() - start;

    // Set up last_Java_sp and last_Java_fp
    address the_pc = __ pc();
    __ set_last_Java_frame(sp, fp, the_pc, t0);

    // Call runtime
    if (arg1 != noreg) {
      assert(arg2 != c_rarg1, "clobbered");
      __ mv(c_rarg1, arg1);
    }
    if (arg2 != noreg) {
      __ mv(c_rarg2, arg2);
    }
    __ mv(c_rarg0, xthread);
    BLOCK_COMMENT("call runtime_entry");
    int32_t offset = 0;
    __ movptr_with_offset(t0, runtime_entry, offset);
    __ jalr(x1, t0, offset);

    // Generate oop map
    OopMap* map = new OopMap(framesize, 0);
    assert_cond(map != NULL);

    oop_maps->add_gc_map(the_pc - start, map);

    __ reset_last_Java_frame(true);
    __ ifence();

    __ leave();

    // check for pending exceptions
#ifdef ASSERT
    Label L;
    __ ld(t0, Address(xthread, Thread::pending_exception_offset()));
    __ bnez(t0, L);
    __ should_not_reach_here();
    __ bind(L);
#endif // ASSERT
    __ far_jump(RuntimeAddress(StubRoutines::forward_exception_entry()));


    // codeBlob framesize is in words (not VMRegImpl::slot_size)
    RuntimeStub* stub =
      RuntimeStub::new_runtime_stub(name,
                                    &code,
                                    frame_complete,
                                    (framesize >> (LogBytesPerWord - LogBytesPerInt)),
                                    oop_maps, false);
    assert(stub != NULL, "create runtime stub fail!");
    return stub->entry_point();
  }

#ifdef COMPILER2
  class MontgomeryMultiplyGenerator : public MacroAssembler {

    Register Pa_base, Pb_base, Pn_base, Pm_base, inv, Rlen, Ra, Rb, Rm, Rn,
      Pa, Pb, Pn, Pm, Rhi_ab, Rlo_ab, Rhi_mn, Rlo_mn, tmp, tmp1, tmp2, Ri, Rj;

    RegSet _toSave;
    bool _squaring;

  public:
    MontgomeryMultiplyGenerator (Assembler *as, bool squaring)
      : MacroAssembler(as->code()), _squaring(squaring) {

      // Register allocation

      Register reg = c_rarg0;
      Pa_base = reg;       // Argument registers
      if (squaring) {
        Pb_base = Pa_base;
      } else {
        Pb_base = ++reg;
      }
      Pn_base = ++reg;
      Rlen= ++reg;
      inv = ++reg;
      Pm_base = ++reg;

                        // Working registers:
      Ra =  ++reg;      // The current digit of a, b, n, and m.
      Rb =  ++reg;
      Rm =  ++reg;
      Rn =  ++reg;

      Pa =  ++reg;      // Pointers to the current/next digit of a, b, n, and m.
      Pb =  ++reg;
      Pm =  ++reg;
      Pn =  ++reg;

      tmp  =  ++reg;    // Three registers which form a
      tmp1 =  ++reg;    // triple-precision accumuator.
      tmp2 =  ++reg;

      Ri =  x6;         // Inner and outer loop indexes.
      Rj =  x7;

      Rhi_ab = x28;     // Product registers: low and high parts
      Rlo_ab = x29;     // of a*b and m*n.
      Rhi_mn = x30;
      Rlo_mn = x31;

      // x18 and up are callee-saved.
      _toSave = RegSet::range(x18, reg) + Pm_base;
    }

  private:
    void save_regs() {
      push_reg(_toSave, sp);
    }

    void restore_regs() {
      pop_reg(_toSave, sp);
    }

    template <typename T>
    void unroll_2(Register count, T block) {
      Label loop, end, odd;
      beqz(count, end);
      andi(t0, count, 0x1);
      bnez(t0, odd);
      align(16);
      bind(loop);
      (this->*block)();
      bind(odd);
      (this->*block)();
      addi(count, count, -2);
      bgtz(count, loop);
      bind(end);
    }

    template <typename T>
    void unroll_2(Register count, T block, Register d, Register s, Register tmp) {
      Label loop, end, odd;
      beqz(count, end);
      andi(tmp, count, 0x1);
      bnez(tmp, odd);
      align(16);
      bind(loop);
      (this->*block)(d, s, tmp);
      bind(odd);
      (this->*block)(d, s, tmp);
      addi(count, count, -2);
      bgtz(count, loop);
      bind(end);
    }

    void pre1(RegisterOrConstant i) {
      block_comment("pre1");
      // Pa = Pa_base;
      // Pb = Pb_base + i;
      // Pm = Pm_base;
      // Pn = Pn_base + i;
      // Ra = *Pa;
      // Rb = *Pb;
      // Rm = *Pm;
      // Rn = *Pn;
      if (i.is_register()) {
        slli(t0, i.as_register(), LogBytesPerWord);
      } else {
        mv(t0, i.as_constant());
        slli(t0, t0, LogBytesPerWord);
      }

      mv(Pa, Pa_base);
      add(Pb, Pb_base, t0);
      mv(Pm, Pm_base);
      add(Pn, Pn_base, t0);

      ld(Ra, Address(Pa));
      ld(Rb, Address(Pb));
      ld(Rm, Address(Pm));
      ld(Rn, Address(Pn));

      // Zero the m*n result.
      mv(Rhi_mn, zr);
      mv(Rlo_mn, zr);
    }

    // The core multiply-accumulate step of a Montgomery
    // multiplication.  The idea is to schedule operations as a
    // pipeline so that instructions with long latencies (loads and
    // multiplies) have time to complete before their results are
    // used.  This most benefits in-order implementations of the
    // architecture but out-of-order ones also benefit.
    void step() {
      block_comment("step");
      // MACC(Ra, Rb, tmp, tmp1, tmp2);
      // Ra = *++Pa;
      // Rb = *--Pb;
      mulhu(Rhi_ab, Ra, Rb);
      mul(Rlo_ab, Ra, Rb);
      addi(Pa, Pa, wordSize);
      ld(Ra, Address(Pa));
      addi(Pb, Pb, -wordSize);
      ld(Rb, Address(Pb));
      acc(Rhi_mn, Rlo_mn, tmp, tmp1, tmp2); // The pending m*n from the
                                            // previous iteration.
      // MACC(Rm, Rn, tmp, tmp1, tmp2);
      // Rm = *++Pm;
      // Rn = *--Pn;
      mulhu(Rhi_mn, Rm, Rn);
      mul(Rlo_mn, Rm, Rn);
      addi(Pm, Pm, wordSize);
      ld(Rm, Address(Pm));
      addi(Pn, Pn, -wordSize);
      ld(Rn, Address(Pn));
      acc(Rhi_ab, Rlo_ab, tmp, tmp1, tmp2);
    }

    void post1() {
      block_comment("post1");

      // MACC(Ra, Rb, tmp, tmp1, tmp2);
      // Ra = *++Pa;
      // Rb = *--Pb;
      mulhu(Rhi_ab, Ra, Rb);
      mul(Rlo_ab, Ra, Rb);
      acc(Rhi_mn, Rlo_mn, tmp, tmp1, tmp2);  // The pending m*n
      acc(Rhi_ab, Rlo_ab, tmp, tmp1, tmp2);

      // *Pm = Rm = tmp * inv;
      mul(Rm, tmp, inv);
      sd(Rm, Address(Pm));

      // MACC(Rm, Rn, tmp, tmp1, tmp2);
      // tmp = tmp1; tmp1 = tmp2; tmp2 = 0;
      mulhu(Rhi_mn, Rm, Rn);

#ifndef PRODUCT
      // assert(m[i] * n[0] + tmp == 0, "broken Montgomery multiply");
      {
        mul(Rlo_mn, Rm, Rn);
        add(Rlo_mn, tmp, Rlo_mn);
        Label ok;
        beqz(Rlo_mn, ok);
        stop("broken Montgomery multiply");
        bind(ok);
      }
#endif
      // We have very carefully set things up so that
      // m[i]*n[0] + tmp == 0 (mod b), so we don't have to calculate
      // the lower half of Rm * Rn because we know the result already:
      // it must be -tmp.  tmp + (-tmp) must generate a carry iff
      // tmp != 0.  So, rather than do a mul and an cad we just set
      // the carry flag iff tmp is nonzero.
      //
      // mul(Rlo_mn, Rm, Rn);
      // cad(zr, tmp, Rlo_mn);
      addi(t0, tmp, -1);
      sltu(t0, t0, tmp); // Set carry iff tmp is nonzero
      cadc(tmp, tmp1, Rhi_mn, t0);
      adc(tmp1, tmp2, zr, t0);
      mv(tmp2, zr);
    }

    void pre2(Register i, Register len) {
      block_comment("pre2");
      // Pa = Pa_base + i-len;
      // Pb = Pb_base + len;
      // Pm = Pm_base + i-len;
      // Pn = Pn_base + len;

      sub(Rj, i, len);
      // Rj == i-len

      // Ra as temp register
      slli(Ra, Rj, LogBytesPerWord);
      add(Pa, Pa_base, Ra);
      add(Pm, Pm_base, Ra);
      slli(Ra, len, LogBytesPerWord);
      add(Pb, Pb_base, Ra);
      add(Pn, Pn_base, Ra);

      // Ra = *++Pa;
      // Rb = *--Pb;
      // Rm = *++Pm;
      // Rn = *--Pn;
      add(Pa, Pa, wordSize);
      ld(Ra, Address(Pa));
      add(Pb, Pb, -wordSize);
      ld(Rb, Address(Pb));
      add(Pm, Pm, wordSize);
      ld(Rm, Address(Pm));
      add(Pn, Pn, -wordSize);
      ld(Rn, Address(Pn));

      mv(Rhi_mn, zr);
      mv(Rlo_mn, zr);
    }

    void post2(Register i, Register len) {
      block_comment("post2");
      sub(Rj, i, len);

      cad(tmp, tmp, Rlo_mn, t0); // The pending m*n, low part

      // As soon as we know the least significant digit of our result,
      // store it.
      // Pm_base[i-len] = tmp;
      // Rj as temp register
      slli(Rj, Rj, LogBytesPerWord);
      add(Rj, Pm_base, Rj);
      sd(tmp, Address(Rj));

      // tmp = tmp1; tmp1 = tmp2; tmp2 = 0;
      cadc(tmp, tmp1, Rhi_mn, t0); // The pending m*n, high part
      adc(tmp1, tmp2, zr, t0);
      mv(tmp2, zr);
    }

    // A carry in tmp after Montgomery multiplication means that we
    // should subtract multiples of n from our result in m.  We'll
    // keep doing that until there is no carry.
    void normalize(Register len) {
      block_comment("normalize");
      // while (tmp)
      //   tmp = sub(Pm_base, Pn_base, tmp, len);
      Label loop, post, again;
      Register cnt = tmp1, i = tmp2; // Re-use registers; we're done with them now
      beqz(tmp, post); {
        bind(again); {
          mv(i, zr);
          mv(cnt, len);
          slli(Rn, i, LogBytesPerWord);
          add(Rm, Pm_base, Rn);
          ld(Rm, Address(Rm));
          add(Rn, Pn_base, Rn);
          ld(Rn, Address(Rn));
          li(t0, 1); // set carry flag, i.e. no borrow
          align(16);
          bind(loop); {
            // csbc(Rm, Rm, Rn);
            notr(Rn, Rn);
            add(Rm, Rm, t0);
            add(Rm, Rm, Rn);
            sltu(t0, Rm, Rn);
            slli(Rn, i, LogBytesPerWord); // Rn as temp register
            add(Rn, Pm_base, Rn);
            sd(Rm, Address(Rn));
            add(i, i, 1);
            slli(Rn, i, LogBytesPerWord);
            add(Rm, Pm_base, Rn);
            ld(Rm, Address(Rm));
            add(Rn, Pn_base, Rn);
            ld(Rn, Address(Rn));
            sub(cnt, cnt, 1);
          } bnez(cnt, loop);
          // sbc(tmp, tmp, zr);
          addi(tmp, tmp, -1);
          add(tmp, tmp, t0);
        } bnez(tmp, again);
      } bind(post);
    }

    // Move memory at s to d, reversing words.
    //    Increments d to end of copied memory
    //    Destroys tmp1, tmp2
    //    Preserves len
    //    Leaves s pointing to the address which was in d at start
    void reverse(Register d, Register s, Register len, Register tmp1, Register tmp2) {
      assert(tmp1 < x28 && tmp2 < x28, "register corruption");

      slli(tmp1, len, LogBytesPerWord);
      add(s, s, tmp1);
      mv(tmp1, len);
      unroll_2(tmp1,  &MontgomeryMultiplyGenerator::reverse1, d, s, tmp2);
      slli(tmp1, len, LogBytesPerWord);
      sub(s, d, tmp1);
    }
    // [63...0] -> [31...0][63...32]
    void reverse1(Register d, Register s, Register tmp) {
      addi(s, s, -wordSize);
      ld(tmp, Address(s));
      ror(tmp, tmp, 32, t0);
      sd(tmp, Address(d));
      addi(d, d, wordSize);
    }

    void step_squaring() {
      // An extra ACC
      step();
      acc(Rhi_ab, Rlo_ab, tmp, tmp1, tmp2);
    }

    void last_squaring(Register i) {
      Label dont;
      // if ((i & 1) == 0) {
      andi(t0, i, 0x1);
      bnez(t0, dont); {
        // MACC(Ra, Rb, tmp, tmp1, tmp2);
        // Ra = *++Pa;
        // Rb = *--Pb;
        mulhu(Rhi_ab, Ra, Rb);
        mul(Rlo_ab, Ra, Rb);
        acc(Rhi_ab, Rlo_ab, tmp, tmp1, tmp2);
      } bind(dont);
    }

    void extra_step_squaring() {
      acc(Rhi_mn, Rlo_mn, tmp, tmp1, tmp2);  // The pending m*n

      // MACC(Rm, Rn, tmp, tmp1, tmp2);
      // Rm = *++Pm;
      // Rn = *--Pn;
      mulhu(Rhi_mn, Rm, Rn);
      mul(Rlo_mn, Rm, Rn);
      addi(Pm, Pm, wordSize);
      ld(Rm, Address(Pm));
      addi(Pn, Pn, -wordSize);
      ld(Rn, Address(Pn));
    }


    void post1_squaring() {
      acc(Rhi_mn, Rlo_mn, tmp, tmp1, tmp2);  // The pending m*n

      // *Pm = Rm = tmp * inv;
      mul(Rm, tmp, inv);
      sd(Rm, Address(Pm));

      // MACC(Rm, Rn, tmp, tmp1, tmp2);
      // tmp = tmp1; tmp1 = tmp2; tmp2 = 0;
      mulhu(Rhi_mn, Rm, Rn);

#ifndef PRODUCT
      // assert(m[i] * n[0] + tmp == 0, "broken Montgomery multiply");
      {
        mul(Rlo_mn, Rm, Rn);
        add(Rlo_mn, tmp, Rlo_mn);
        Label ok;
        beqz(Rlo_mn, ok); {
          stop("broken Montgomery multiply");
        } bind(ok);
      }
#endif
      // We have very carefully set things up so that
      // m[i]*n[0] + tmp == 0 (mod b), so we don't have to calculate
      // the lower half of Rm * Rn because we know the result already:
      // it must be -tmp.  tmp + (-tmp) must generate a carry iff
      // tmp != 0.  So, rather than do a mul and a cad we just set
      // the carry flag iff tmp is nonzero.
      //
      // mul(Rlo_mn, Rm, Rn);
      // cad(zr, tmp, Rlo_mn);
      addi(t0, tmp, -1);
      sltu(t0, t0, tmp); // Set carry iff tmp is nonzero
      cadc(tmp, tmp1, Rhi_mn, t0);
      adc(tmp1, tmp2, zr, t0);
      mv(tmp2, zr);
    }

    // use t0 as carry
    void acc(Register Rhi, Register Rlo,
             Register tmp, Register tmp1, Register tmp2) {
      cad(tmp, tmp, Rlo, t0);
      cadc(tmp1, tmp1, Rhi, t0);
      adc(tmp2, tmp2, zr, t0);
    }

  public:
    /**
     * Fast Montgomery multiplication.  The derivation of the
     * algorithm is in A Cryptographic Library for the Motorola
     * DSP56000, Dusse and Kaliski, Proc. EUROCRYPT 90, pp. 230-237.
     *
     * Arguments:
     *
     * Inputs for multiplication:
     *   c_rarg0   - int array elements a
     *   c_rarg1   - int array elements b
     *   c_rarg2   - int array elements n (the modulus)
     *   c_rarg3   - int length
     *   c_rarg4   - int inv
     *   c_rarg5   - int array elements m (the result)
     *
     * Inputs for squaring:
     *   c_rarg0   - int array elements a
     *   c_rarg1   - int array elements n (the modulus)
     *   c_rarg2   - int length
     *   c_rarg3   - int inv
     *   c_rarg4   - int array elements m (the result)
     *
     */
    address generate_multiply() {
      Label argh, nothing;
      bind(argh);
      stop("MontgomeryMultiply total_allocation must be <= 8192");

      align(CodeEntryAlignment);
      address entry = pc();

      beqz(Rlen, nothing);

      enter();

      // Make room.
      li(Ra, 512);
      bgt(Rlen, Ra, argh);
      slli(Ra, Rlen, exact_log2(4 * sizeof(jint)));
      sub(Ra, sp, Ra);
      andi(sp, Ra, -2 * wordSize);

      srliw(Rlen, Rlen, 1);  // length in longwords = len/2

      {
        // Copy input args, reversing as we go.  We use Ra as a
        // temporary variable.
        reverse(Ra, Pa_base, Rlen, Ri, Rj);
        if (!_squaring)
          reverse(Ra, Pb_base, Rlen, Ri, Rj);
        reverse(Ra, Pn_base, Rlen, Ri, Rj);
      }

      // Push all call-saved registers and also Pm_base which we'll need
      // at the end.
      save_regs();

#ifndef PRODUCT
      // assert(inv * n[0] == -1UL, "broken inverse in Montgomery multiply");
      {
        ld(Rn, Address(Pn_base));
        mul(Rlo_mn, Rn, inv);
        li(t0, -1);
        Label ok;
        beq(Rlo_mn, t0, ok);
        stop("broken inverse in Montgomery multiply");
        bind(ok);
      }
#endif

      mv(Pm_base, Ra);

      mv(tmp, zr);
      mv(tmp1, zr);
      mv(tmp2, zr);

      block_comment("for (int i = 0; i < len; i++) {");
      mv(Ri, zr); {
        Label loop, end;
        bge(Ri, Rlen, end);

        bind(loop);
        pre1(Ri);

        block_comment("  for (j = i; j; j--) {"); {
          mv(Rj, Ri);
          unroll_2(Rj, &MontgomeryMultiplyGenerator::step);
        } block_comment("  } // j");

        post1();
        addw(Ri, Ri, 1);
        blt(Ri, Rlen, loop);
        bind(end);
        block_comment("} // i");
      }

      block_comment("for (int i = len; i < 2*len; i++) {");
      mv(Ri, Rlen); {
        Label loop, end;
        slli(Rj, Rlen, 1); // Rj as temp register
        bge(Ri, Rj, end);

        bind(loop);
        pre2(Ri, Rlen);

        block_comment("  for (j = len*2-i-1; j; j--) {"); {
          slliw(Rj, Rlen, 1);
          subw(Rj, Rj, Ri);
          subw(Rj, Rj, 1);
          unroll_2(Rj, &MontgomeryMultiplyGenerator::step);
        } block_comment("  } // j");

        post2(Ri, Rlen);
        addw(Ri, Ri, 1);
        slli(Rj, Rlen, 1);
        blt(Ri, Rj, loop);
        bind(end);
      }
      block_comment("} // i");


      normalize(Rlen);

      mv(Ra, Pm_base);  // Save Pm_base in Ra
      restore_regs();  // Restore caller's Pm_base

      // Copy our result into caller's Pm_base
      reverse(Pm_base, Ra, Rlen, Ri, Rj);

      leave();
      bind(nothing);
      ret();

      return entry;
    }

    /**
     *
     * Arguments:
     *
     * Inputs:
     *   c_rarg0   - int array elements a
     *   c_rarg1   - int array elements n (the modulus)
     *   c_rarg2   - int length
     *   c_rarg3   - int inv
     *   c_rarg4   - int array elements m (the result)
     *
     */
    address generate_square() {
      Label argh;
      bind(argh);
      stop("MontgomeryMultiply total_allocation must be <= 8192");

      align(CodeEntryAlignment);
      address entry = pc();

      enter();

      // Make room.
      li(Ra, 512);
      bgt(Rlen, Ra, argh);
      slli(Ra, Rlen, exact_log2(4 * sizeof(jint)));
      sub(Ra, sp, Ra);
      andi(sp, Ra, -2 * wordSize);

      srliw(Rlen, Rlen, 1);  // length in longwords = len/2

      {
        // Copy input args, reversing as we go.  We use Ra as a
        // temporary variable.
        reverse(Ra, Pa_base, Rlen, Ri, Rj);
        reverse(Ra, Pn_base, Rlen, Ri, Rj);
      }

      // Push all call-saved registers and also Pm_base which we'll need
      // at the end.
      save_regs();

      mv(Pm_base, Ra);

      mv(tmp, zr);
      mv(tmp1, zr);
      mv(tmp2, zr);

      block_comment("for (int i = 0; i < len; i++) {");
      mv(Ri, zr); {
        Label loop, end;
        bind(loop);
        bge(Ri, Rlen, end);

        pre1(Ri);

        block_comment("for (j = (i+1)/2; j; j--) {"); {
          addi(Rj, Ri, 1);
          srliw(Rj, Rj, 1);
          unroll_2(Rj, &MontgomeryMultiplyGenerator::step_squaring);
        } block_comment("  } // j");

        last_squaring(Ri);

        block_comment("  for (j = i/2; j; j--) {"); {
          srliw(Rj, Ri, 1);
          unroll_2(Rj, &MontgomeryMultiplyGenerator::extra_step_squaring);
        } block_comment("  } // j");

        post1_squaring();
        addi(Ri, Ri, 1);
        blt(Ri, Rlen, loop);

        bind(end);
        block_comment("} // i");
      }

      block_comment("for (int i = len; i < 2*len; i++) {");
      mv(Ri, Rlen); {
        Label loop, end;
        bind(loop);
        slli(Rj, Rlen, 1);
        bge(Ri, Rj, end);

        pre2(Ri, Rlen);

        block_comment("  for (j = (2*len-i-1)/2; j; j--) {"); {
          slli(Rj, Rlen, 1);
          sub(Rj, Rj, Ri);
          sub(Rj, Rj, 1);
          srliw(Rj, Rj, 1);
          unroll_2(Rj, &MontgomeryMultiplyGenerator::step_squaring);
        } block_comment("  } // j");

        last_squaring(Ri);

        block_comment("  for (j = (2*len-i)/2; j; j--) {"); {
          slli(Rj, Rlen, 1);
          sub(Rj, Rj, Ri);
          srliw(Rj, Rj, 1);
          unroll_2(Rj, &MontgomeryMultiplyGenerator::extra_step_squaring);
        } block_comment("  } // j");

        post2(Ri, Rlen);
        addi(Ri, Ri, 1);
        slli(t0, Rlen, 1);
        blt(Ri, t0, loop);

        bind(end);
        block_comment("} // i");
      }

      normalize(Rlen);

      mv(Ra, Pm_base);  // Save Pm_base in Ra
      restore_regs();  // Restore caller's Pm_base

      // Copy our result into caller's Pm_base
      reverse(Pm_base, Ra, Rlen, Ri, Rj);

      leave();
      ret();

      return entry;
    }
  };
#endif // COMPILER2

  // Initialization
  void generate_initial() {
    // Generate initial stubs and initializes the entry points

    // entry points that exist in all platforms Note: This is code
    // that could be shared among different platforms - however the
    // benefit seems to be smaller than the disadvantage of having a
    // much more complicated generator structure. See also comment in
    // stubRoutines.hpp.

    StubRoutines::_forward_exception_entry = generate_forward_exception();

    StubRoutines::_call_stub_entry =
      generate_call_stub(StubRoutines::_call_stub_return_address);

    // is referenced by megamorphic call
    StubRoutines::_catch_exception_entry = generate_catch_exception();

    // Build this early so it's available for the interpreter.
    StubRoutines::_throw_StackOverflowError_entry =
      generate_throw_exception("StackOverflowError throw_exception",
                               CAST_FROM_FN_PTR(address,
                                                SharedRuntime::throw_StackOverflowError));
    StubRoutines::_throw_delayed_StackOverflowError_entry =
      generate_throw_exception("delayed StackOverflowError throw_exception",
                               CAST_FROM_FN_PTR(address,
                                                SharedRuntime::throw_delayed_StackOverflowError));
  }

  void generate_all() {
    // support for verify_oop (must happen after universe_init)
    StubRoutines::_verify_oop_subroutine_entry     = generate_verify_oop();
    StubRoutines::_throw_AbstractMethodError_entry =
      generate_throw_exception("AbstractMethodError throw_exception",
                               CAST_FROM_FN_PTR(address,
                                                SharedRuntime::
                                                throw_AbstractMethodError));

    StubRoutines::_throw_IncompatibleClassChangeError_entry =
      generate_throw_exception("IncompatibleClassChangeError throw_exception",
                               CAST_FROM_FN_PTR(address,
                                                SharedRuntime::
                                                throw_IncompatibleClassChangeError));

    StubRoutines::_throw_NullPointerException_at_call_entry =
      generate_throw_exception("NullPointerException at call throw_exception",
                               CAST_FROM_FN_PTR(address,
                                                SharedRuntime::
                                                throw_NullPointerException_at_call));
    // arraycopy stubs used by compilers
    generate_arraycopy_stubs();

#ifdef COMPILER2
    if (UseMulAddIntrinsic) {
      StubRoutines::_mulAdd = generate_mulAdd();
    }

    if (UseMultiplyToLenIntrinsic) {
      StubRoutines::_multiplyToLen = generate_multiplyToLen();
    }

    if (UseSquareToLenIntrinsic) {
      StubRoutines::_squareToLen = generate_squareToLen();
    }

    generate_compare_long_strings();

    generate_string_indexof_stubs();

    if (UseMontgomeryMultiplyIntrinsic) {
      StubCodeMark mark(this, "StubRoutines", "montgomeryMultiply");
      MontgomeryMultiplyGenerator g(_masm, /*squaring*/false);
      StubRoutines::_montgomeryMultiply = g.generate_multiply();
    }

    if (UseMontgomerySquareIntrinsic) {
      StubCodeMark mark(this, "StubRoutines", "montgomerySquare");
      MontgomeryMultiplyGenerator g(_masm, /*squaring*/true);
      StubRoutines::_montgomerySquare = g.generate_square();
    }
#endif // COMPILER2
    // Safefetch stubs.
    generate_safefetch("SafeFetch32", sizeof(int),     &StubRoutines::_safefetch32_entry,
                                                       &StubRoutines::_safefetch32_fault_pc,
                                                       &StubRoutines::_safefetch32_continuation_pc);
    generate_safefetch("SafeFetchN", sizeof(intptr_t), &StubRoutines::_safefetchN_entry,
                                                       &StubRoutines::_safefetchN_fault_pc,
                                                       &StubRoutines::_safefetchN_continuation_pc);

    StubRoutines::riscv64::set_completed();
  }

 public:
  StubGenerator(CodeBuffer* code, bool all) : StubCodeGenerator(code) {
    if (all) {
      generate_all();
    } else {
      generate_initial();
    }
  }

  ~StubGenerator() {}
}; // end class declaration

void StubGenerator_generate(CodeBuffer* code, bool all) {
  StubGenerator g(code, all);
}
