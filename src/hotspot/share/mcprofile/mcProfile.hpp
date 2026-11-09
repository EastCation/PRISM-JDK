/*
 * Copyright (c) 2026, MoeRain Software Developments. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 */

#ifndef SHARE_MCPROFILE_MCPROFILE_HPP
#define SHARE_MCPROFILE_MCPROFILE_HPP

#include "memory/allocation.hpp"
#include "runtime/handles.hpp"
#include "utilities/globalDefinitions.hpp"

class Method;

// PRISM-MCJDK profile subsystem.
//
// This is the HotSpot-side counterpart of the ART profile system.  It records
// method hotness (invocation/backedge counters), the highest compilation level
// and code size for methods that were active in a run.  The recorded data is
// loaded into a lookup index at startup and will be used to seed tiered
// compilation and profile-guided AOT in the next stages.
class MCMethodProfile : public AllStatic {
 public:
  static void initialize();
  static void on_vm_exit();
  static void note_compile_request(const methodHandle& method, int comp_level);
  static bool is_enabled() { return MCProfile; }
  static bool should_use();
  static bool should_record();

  static void dump_method(Method* method);

 private:
  static bool mode_is(const char* name);
  static bool mode_off();
};

#endif // SHARE_MCPROFILE_MCPROFILE_HPP
