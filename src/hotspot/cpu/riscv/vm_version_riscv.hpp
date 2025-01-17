/*
 * Copyright (c) 1997, 2016, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2020, 2021, Huawei Technologies Co., Ltd. All rights reserved.
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

#ifndef CPU_RISCV_VM_VERSION_RISCV_HPP
#define CPU_RISCV_VM_VERSION_RISCV_HPP

#include "runtime/abstract_vm_version.hpp"
#include "runtime/globals_extension.hpp"
#include "utilities/sizes.hpp"

class VM_Version : public Abstract_VM_Version {
public:
  // Initialization
  static void initialize();

  enum Feature_Flag {
#define CPU_FEATURE_FLAGS(decl)               \
    decl(I,            "I",            8)     \
    decl(M,            "M",           12)     \
    decl(A,            "A",            0)     \
    decl(F,            "F",            5)     \
    decl(D,            "D",            3)     \
    decl(C,            "C",            2)     \
    decl(V,            "V",           21)     \
    decl(B,            "B",            1)

#define DECLARE_CPU_FEATURE_FLAG(id, name, bit) CPU_##id = (1 << bit),
    CPU_FEATURE_FLAGS(DECLARE_CPU_FEATURE_FLAG)
#undef DECLARE_CPU_FEATURE_FLAG
  };

protected:
  static uint32_t _initial_vector_length;
  static void get_processor_features();
  static void get_cpu_info();
  static uint32_t get_current_vector_length();

#ifdef COMPILER2
private:
  static void get_c2_processor_features();
#endif // COMPILER2
};

#endif // CPU_RISCV_VM_VERSION_RISCV_HPP
