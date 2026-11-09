/*
 * Copyright (c) 2026, MoeRain Software Developments. All rights reserved.
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
 */

#ifndef SHARE_MCCACHE_MCNATIVECACHE_HPP
#define SHARE_MCCACHE_MCNATIVECACHE_HPP

#include "memory/allocation.hpp"
#include "runtime/handles.hpp"
#include "utilities/globalDefinitions.hpp"

class Method;
class nmethod;

// PRISM-MCJDK native method cache.
//
// This is the first stage of the persistent nmethod cache.  It records the
// identity and raw code blob of every JIT-compiled Java method at VM exit and
// validates/loads the index at VM startup.  The restore path is intentionally
// conservative for now: a cache hit is reported, but compilation still falls
// back to the normal JIT pipeline until the relocation and metadata restore
// code is enabled in the next stage.
class MCNativeMethodCache : public AllStatic {
 public:
  static void initialize();
  static void on_vm_exit();

  // Called before a method is handed to the JIT.  Returns a restored nmethod
  // once the P2 restore path is enabled; currently returns nullptr.
  static nmethod* try_restore(const methodHandle& method, int osr_bci, int comp_level);

  static bool is_enabled() { return MCNativeCache; }
  static bool should_use();
  static bool should_record();

  // Used by CodeCache::nmethods_do() during a dump.
  static void dump_nmethod(nmethod* nm);

 private:
  static bool mode_is(const char* name);
  static bool mode_off();
};

#endif // SHARE_MCCACHE_MCNATIVECACHE_HPP
