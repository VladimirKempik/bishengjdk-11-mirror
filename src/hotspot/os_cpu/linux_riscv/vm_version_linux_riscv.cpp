/*
 * Copyright (c) 2006, 2021, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2021, Huawei Technologies Co., Ltd. All rights reserved.
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
#include "asm/register.hpp"
#include "runtime/os.hpp"
#include "runtime/os.inline.hpp"
#include "runtime/vm_version.hpp"

#include <asm/hwcap.h>
#include <sys/auxv.h>

#ifndef HWCAP_ISA_I
#define HWCAP_ISA_I  (1 << ('I' - 'A'))
#endif

#ifndef HWCAP_ISA_M
#define HWCAP_ISA_M  (1 << ('M' - 'A'))
#endif

#ifndef HWCAP_ISA_A
#define HWCAP_ISA_A  (1 << ('A' - 'A'))
#endif

#ifndef HWCAP_ISA_F
#define HWCAP_ISA_F  (1 << ('F' - 'A'))
#endif

#ifndef HWCAP_ISA_D
#define HWCAP_ISA_D  (1 << ('D' - 'A'))
#endif

#ifndef HWCAP_ISA_C
#define HWCAP_ISA_C  (1 << ('C' - 'A'))
#endif

#ifndef HWCAP_ISA_V
#define HWCAP_ISA_V  (1 << ('V' - 'A'))
#endif

#ifndef HWCAP_ISA_B
#define HWCAP_ISA_B  (1 << ('B' - 'A'))
#endif

#define read_csr(csr)                                           \
({                                                              \
        register unsigned long __v;                             \
        __asm__ __volatile__ ("csrr %0, %1"                     \
                              : "=r" (__v)                      \
                              : "i" (csr)                       \
                              : "memory");                      \
        __v;                                                    \
})

uint32_t VM_Version::get_current_vector_length() {
  assert(_features & CPU_V, "should not call this");
  return (uint32_t)read_csr(CSR_VLENB);
}

void VM_Version::get_cpu_info() {

  uint64_t auxv = getauxval(AT_HWCAP);

  STATIC_ASSERT(CPU_I == HWCAP_ISA_I);
  STATIC_ASSERT(CPU_M == HWCAP_ISA_M);
  STATIC_ASSERT(CPU_A == HWCAP_ISA_A);
  STATIC_ASSERT(CPU_F == HWCAP_ISA_F);
  STATIC_ASSERT(CPU_D == HWCAP_ISA_D);
  STATIC_ASSERT(CPU_C == HWCAP_ISA_C);
  STATIC_ASSERT(CPU_V == HWCAP_ISA_V);
  STATIC_ASSERT(CPU_B == HWCAP_ISA_B);
  _features = auxv & (
      HWCAP_ISA_I |
      HWCAP_ISA_M |
      HWCAP_ISA_A |
      HWCAP_ISA_F |
      HWCAP_ISA_D |
      HWCAP_ISA_C |
      HWCAP_ISA_V |
      HWCAP_ISA_B);
}
