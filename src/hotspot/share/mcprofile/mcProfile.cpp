/*
 * Copyright (c) 2026, MoeRain Software Developments. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 */

#include "precompiled.hpp"
#include "classfile/classLoaderData.inline.hpp"
#include "classfile/classLoaderDataGraph.hpp"
#include "code/compiledMethod.hpp"
#include "compiler/compilerDefinitions.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/resourceArea.hpp"
#include "mcprofile/mcProfile.hpp"
#include "oops/constMethod.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/method.hpp"
#include "oops/symbol.hpp"
#include "runtime/abstract_vm_version.hpp"
#include "runtime/globals.hpp"
#include "runtime/os.hpp"

#include <stdio.h>
#include <string.h>

#define MC_PROFILE_MAGIC          0x4D435052u   // "MCPR"
#define MC_PROFILE_RECORD_MAGIC   0x4D435031u   // "MCP1"
#define MC_PROFILE_VERSION        1u

#define MC_PROFILE_LOG(...) \
  do { if (MCProfileVerbose) { tty->print_cr(__VA_ARGS__); } } while (0)

enum {
  MC_PROFILE_LOADER_BOOT     = 0,
  MC_PROFILE_LOADER_PLATFORM = 1,
  MC_PROFILE_LOADER_SYSTEM   = 2,
  MC_PROFILE_LOADER_CUSTOM   = 3
};

struct MCProfileFileHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t pointer_size;
  uint32_t compressed_oops;
  uint64_t vm_fingerprint;
  uint64_t record_count;
  uint64_t file_size;
};

struct MCProfileRecordHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t comp_level;
  uint32_t highest_comp_level;
  uint32_t loader_kind;
  uint32_t flags;
  uint32_t class_name_len;
  uint32_t method_name_len;
  uint32_t signature_len;
  uint64_t method_fingerprint;
  uint64_t invocation_count;
  uint64_t backedge_count;
  uint64_t code_size;
};

class MCProfileEntry : public CHeapObj<mtInternal> {
 public:
  MCProfileEntry* _next;
  char*           _class_name;
  char*           _method_name;
  char*           _signature;
  uint64_t        _method_fingerprint;
  uint64_t        _invocation_count;
  uint64_t        _backedge_count;
  uint64_t        _code_size;
  uint32_t        _comp_level;
  uint32_t        _highest_comp_level;
  uint32_t        _loader_kind;
  uint32_t        _flags;

  MCProfileEntry() :
      _next(nullptr), _class_name(nullptr), _method_name(nullptr),
      _signature(nullptr), _method_fingerprint(0), _invocation_count(0),
      _backedge_count(0), _code_size(0), _comp_level(0),
      _highest_comp_level(0), _loader_kind(MC_PROFILE_LOADER_BOOT), _flags(0) {}

  ~MCProfileEntry() {
    FREE_C_HEAP_ARRAY(char, _class_name);
    FREE_C_HEAP_ARRAY(char, _method_name);
    FREE_C_HEAP_ARRAY(char, _signature);
  }
};

static MCProfileEntry* _entries = nullptr;
static uint64_t        _entry_count = 0;
static bool            _use_disabled = false;
static FILE*           _dump_file = nullptr;
static uint64_t        _dump_count = 0;

static uint64_t fnv1a(const void* data, size_t len, uint64_t h) {
  const uint8_t* p = (const uint8_t*) data;
  for (size_t i = 0; i < len; i++) {
    h ^= (uint64_t) p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static uint64_t vm_fingerprint() {
  uint64_t h = 1469598103934665603ULL;
  const char* release = Abstract_VM_Version::vm_release();
  if (release != nullptr) {
    h = fnv1a(release, strlen(release), h);
  }
  uint32_t values[] = {
    (uint32_t) UseCompressedOops,
    (uint32_t) Abstract_VM_Version::vm_major_version(),
    (uint32_t) Abstract_VM_Version::vm_minor_version(),
    (uint32_t) Abstract_VM_Version::vm_security_version(),
    (uint32_t) Abstract_VM_Version::vm_patch_version(),
    (uint32_t) Abstract_VM_Version::vm_build_number()
  };
  return fnv1a(values, sizeof(values), h);
}

static uint32_t loader_kind(InstanceKlass* holder) {
  ClassLoaderData* cld = holder->class_loader_data();
  if (cld->is_boot_class_loader_data()) return MC_PROFILE_LOADER_BOOT;
  if (cld->is_platform_class_loader_data()) return MC_PROFILE_LOADER_PLATFORM;
  if (cld->is_system_class_loader_data()) return MC_PROFILE_LOADER_SYSTEM;
  return MC_PROFILE_LOADER_CUSTOM;
}

bool MCMethodProfile::mode_is(const char* name) {
  return MCProfileMode != nullptr && strcmp(MCProfileMode, name) == 0;
}

bool MCMethodProfile::mode_off() {
  return MCProfileMode == nullptr || strcmp(MCProfileMode, "off") == 0;
}

bool MCMethodProfile::should_use() {
  return MCProfile && !_use_disabled &&
         (mode_off() || mode_is("use") || mode_is("both") || mode_is("auto"));
}

bool MCMethodProfile::should_record() {
  return MCProfile &&
         (mode_off() || mode_is("record") || mode_is("both") || mode_is("auto"));
}

void MCMethodProfile::initialize() {
  if (!MCProfile || !should_use()) {
    return;
  }
  if (MCProfilePath == nullptr || MCProfilePath[0] == '\0') {
    _use_disabled = true;
    return;
  }

  FILE* fp = os::fopen(MCProfilePath, "rb");
  if (fp == nullptr) {
    _use_disabled = true;
    return;
  }

  MCProfileFileHeader header;
  if (fread(&header, sizeof(header), 1, fp) != 1 ||
      header.magic != MC_PROFILE_MAGIC ||
      header.version != MC_PROFILE_VERSION ||
      header.pointer_size != (uint32_t) sizeof(void*) ||
      header.compressed_oops != (UseCompressedOops ? 1u : 0u) ||
      header.vm_fingerprint != vm_fingerprint()) {
    fclose(fp);
    _use_disabled = true;
    return;
  }

  uint64_t count = header.record_count;
  if (count > (uint64_t) MCProfileMaxMethods) {
    count = (uint64_t) MCProfileMaxMethods;
  }

  for (uint64_t i = 0; i < count; i++) {
    MCProfileRecordHeader rec;
    if (fread(&rec, sizeof(rec), 1, fp) != 1 ||
        rec.magic != MC_PROFILE_RECORD_MAGIC ||
        rec.version != MC_PROFILE_VERSION) {
      break;
    }

    char* cn = NEW_C_HEAP_ARRAY(char, (size_t) rec.class_name_len + 1, mtInternal);
    char* mn = NEW_C_HEAP_ARRAY(char, (size_t) rec.method_name_len + 1, mtInternal);
    char* sg = NEW_C_HEAP_ARRAY(char, (size_t) rec.signature_len + 1, mtInternal);
    bool ok = fread(cn, 1, rec.class_name_len, fp) == rec.class_name_len &&
              fread(mn, 1, rec.method_name_len, fp) == rec.method_name_len &&
              fread(sg, 1, rec.signature_len, fp) == rec.signature_len;
    cn[rec.class_name_len] = '\0';
    mn[rec.method_name_len] = '\0';
    sg[rec.signature_len] = '\0';
    if (!ok) {
      FREE_C_HEAP_ARRAY(char, cn);
      FREE_C_HEAP_ARRAY(char, mn);
      FREE_C_HEAP_ARRAY(char, sg);
      break;
    }

    MCProfileEntry* e = new MCProfileEntry();
    e->_class_name = cn;
    e->_method_name = mn;
    e->_signature = sg;
    e->_method_fingerprint = rec.method_fingerprint;
    e->_invocation_count = rec.invocation_count;
    e->_backedge_count = rec.backedge_count;
    e->_code_size = rec.code_size;
    e->_comp_level = rec.comp_level;
    e->_highest_comp_level = rec.highest_comp_level;
    e->_loader_kind = rec.loader_kind;
    e->_flags = rec.flags;
    e->_next = _entries;
    _entries = e;
    _entry_count++;
  }

  fclose(fp);
  MC_PROFILE_LOG("MCProfile: loaded " UINT64_FORMAT " methods from %s",
                 _entry_count, MCProfilePath);
}

void MCMethodProfile::dump_method(Method* m) {
  if (_dump_file == nullptr || _dump_count >= (uint64_t) MCProfileMaxMethods ||
      m == nullptr || m->is_native() || m->is_abstract()) {
    return;
  }

  int inv = m->invocation_count();
  int be = m->backedge_count();
  int highest = m->highest_comp_level();
  if (inv == 0 && be == 0 && highest == CompLevel_none) {
    return;
  }

  ResourceMark rm;
  InstanceKlass* holder = m->method_holder();
  char* class_name = holder->name()->as_C_string();
  char* method_name = m->name()->as_C_string();
  char* signature = m->signature()->as_C_string();

  CompiledMethod* cm = m->code();
  uint64_t code_size = cm != nullptr ? (uint64_t) cm->code_size() : 0;

  MCProfileRecordHeader rec;
  memset(&rec, 0, sizeof(rec));
  rec.magic = MC_PROFILE_RECORD_MAGIC;
  rec.version = MC_PROFILE_VERSION;
  rec.comp_level = cm != nullptr ? (uint32_t) cm->comp_level() : 0;
  rec.highest_comp_level = (uint32_t) highest;
  rec.loader_kind = loader_kind(holder);
  rec.class_name_len = (uint32_t) strlen(class_name);
  rec.method_name_len = (uint32_t) strlen(method_name);
  rec.signature_len = (uint32_t) strlen(signature);
  rec.method_fingerprint = m->constMethod()->fingerprint();
  rec.invocation_count = (uint64_t) inv;
  rec.backedge_count = (uint64_t) be;
  rec.code_size = code_size;

  bool ok = fwrite(&rec, sizeof(rec), 1, _dump_file) == 1 &&
            fwrite(class_name, 1, rec.class_name_len, _dump_file) == rec.class_name_len &&
            fwrite(method_name, 1, rec.method_name_len, _dump_file) == rec.method_name_len &&
            fwrite(signature, 1, rec.signature_len, _dump_file) == rec.signature_len;
  if (ok) {
    _dump_count++;
  }
}

void MCMethodProfile::on_vm_exit() {
  if (!should_record()) {
    return;
  }
  if (MCProfilePath == nullptr || MCProfilePath[0] == '\0') {
    return;
  }

  FILE* fp = os::fopen(MCProfilePath, "wb");
  if (fp == nullptr) {
    return;
  }
  _dump_file = fp;
  _dump_count = 0;

  MCProfileFileHeader header;
  memset(&header, 0, sizeof(header));
  header.magic = MC_PROFILE_MAGIC;
  header.version = MC_PROFILE_VERSION;
  header.pointer_size = (uint32_t) sizeof(void*);
  header.compressed_oops = UseCompressedOops ? 1u : 0u;
  header.vm_fingerprint = vm_fingerprint();
  fwrite(&header, sizeof(header), 1, fp);

  ClassLoaderDataGraph::methods_do(dump_method);

  long end = ftell(fp);
  header.record_count = _dump_count;
  header.file_size = end > 0 ? (uint64_t) end : 0;
  fseek(fp, 0, SEEK_SET);
  fwrite(&header, sizeof(header), 1, fp);
  fclose(fp);
  _dump_file = nullptr;

  MC_PROFILE_LOG("MCProfile: recorded " UINT64_FORMAT " methods to %s",
                 _dump_count, MCProfilePath);
}

void MCMethodProfile::note_compile_request(const methodHandle& method, int comp_level) {
  if (!should_use() || _entry_count == 0) {
    return;
  }

  Method* m = method();
  InstanceKlass* holder = m->method_holder();
  uint32_t kind = loader_kind(holder);
  uint64_t fp = m->constMethod()->fingerprint();

  ResourceMark rm;
  const char* class_name = holder->name()->as_C_string();
  const char* method_name = m->name()->as_C_string();
  const char* signature = m->signature()->as_C_string();

  for (MCProfileEntry* e = _entries; e != nullptr; e = e->_next) {
    if (e->_loader_kind != kind || e->_method_fingerprint != fp) {
      continue;
    }
    if (strcmp(e->_class_name, class_name) != 0 ||
        strcmp(e->_method_name, method_name) != 0 ||
        strcmp(e->_signature, signature) != 0) {
      continue;
    }
    MC_PROFILE_LOG("MCProfile: HIT %s::%s%s level=%d prevLevel=%d "
                   "inv=" UINT64_FORMAT " be=" UINT64_FORMAT,
                   class_name, method_name, signature, comp_level,
                   (int) e->_highest_comp_level,
                   e->_invocation_count, e->_backedge_count);
    return;
  }
}
