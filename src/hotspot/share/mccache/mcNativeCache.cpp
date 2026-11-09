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

#include "precompiled.hpp"
#include "classfile/classLoaderData.inline.hpp"
#include "code/codeCache.hpp"
#include "code/nmethod.hpp"
#include "code/relocInfo.hpp"
#include "compiler/compileBroker.hpp"
#include "compiler/oopMap.hpp"
#include "compiler/compilerDefinitions.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/resourceArea.hpp"
#include "mccache/mcNativeCache.hpp"
#include "oops/constMethod.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/method.hpp"
#include "oops/symbol.hpp"
#include "runtime/abstract_vm_version.hpp"
#include "runtime/globals.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.hpp"
#include "runtime/sharedRuntime.hpp"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MC_NATIVE_CACHE_MAGIC          0x4D434E43u   // "MCNC"
#define MC_NATIVE_CACHE_RECORD_MAGIC   0x4D435231u   // "MCR1"
#define MC_NATIVE_CACHE_VERSION        4u

#define MC_NATIVE_CACHE_LOG(...) \
  do { if (MCNativeCacheVerbose) { tty->print_cr(__VA_ARGS__); } } while (0)

// Symbolic loader buckets.  A future revision will record a stable class
// loader fingerprint for custom loaders; until then only the built-in loader
// buckets are considered safe for a cache hit.
enum {
  MC_LOADER_BOOT      = 0,
  MC_LOADER_PLATFORM  = 1,
  MC_LOADER_SYSTEM    = 2,
  MC_LOADER_CUSTOM    = 3
};

struct MCNativeCacheFileHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t pointer_size;
  uint32_t compressed_oops;
  uint32_t compressed_class_pointers;
  uint32_t use_g1;
  uint32_t use_avx;
  uint32_t use_sse;
  uint32_t use_fma;
  uint32_t vm_major;
  uint32_t vm_minor;
  uint32_t vm_security;
  uint32_t vm_patch;
  uint32_t vm_build;
  uint64_t vm_fingerprint;
  uint64_t record_count;
  uint64_t data_offset;
  uint64_t file_size;
  uint64_t ic_miss_stub_begin;
  uint64_t ic_miss_stub_size;
};

struct MCNativeCacheRecordHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t comp_level;
  uint32_t flags;
  uint32_t loader_kind;
  uint32_t class_name_len;
  uint32_t method_name_len;
  uint32_t signature_len;
  uint64_t method_fingerprint;
  uint64_t code_size;
  uint64_t blob_size;
  uint64_t blob_offset;
  uint64_t oopmap_size;
  uint64_t oopmap_offset;
  uint64_t entry_bci;
};

class MCNativeCacheEntry : public CHeapObj<mtInternal> {
 public:
  MCNativeCacheEntry* _next;
  char*               _class_name;
  char*               _method_name;
  char*               _signature;
  uint64_t            _method_fingerprint;
  uint64_t            _code_size;
  uint64_t            _blob_size;
  uint64_t            _blob_offset;
  uint64_t            _oopmap_size;
  uint64_t            _oopmap_offset;
  uint64_t            _entry_bci;
  uint32_t            _comp_level;
  uint32_t            _flags;
  uint32_t            _loader_kind;

  MCNativeCacheEntry() :
      _next(nullptr),
      _class_name(nullptr),
      _method_name(nullptr),
      _signature(nullptr),
      _method_fingerprint(0),
      _code_size(0),
      _blob_size(0),
      _blob_offset(0),
      _oopmap_size(0),
      _oopmap_offset(0),
      _entry_bci(0),
      _comp_level(0),
      _flags(0),
      _loader_kind(MC_LOADER_BOOT) {}

  ~MCNativeCacheEntry() {
    FREE_C_HEAP_ARRAY(char, _class_name);
    FREE_C_HEAP_ARRAY(char, _method_name);
    FREE_C_HEAP_ARRAY(char, _signature);
  }
};

static MCNativeCacheEntry* _entries = nullptr;
static uint64_t            _entry_count = 0;
static bool                _use_disabled = false;
static uint64_t            _saved_ic_miss_begin = 0;
static uint64_t            _saved_ic_miss_size = 0;

static FILE*               _dump_file = nullptr;
static uint64_t            _dump_count = 0;
static int                 _safe_count = 0;
static int                 _detail_count = 0;

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
    (uint32_t) UseCompressedClassPointers,
    (uint32_t) UseG1GC,
    (uint32_t) UseAVX,
    (uint32_t) UseSSE,
    (uint32_t) UseFMA,
    (uint32_t) Abstract_VM_Version::vm_major_version(),
    (uint32_t) Abstract_VM_Version::vm_minor_version(),
    (uint32_t) Abstract_VM_Version::vm_security_version(),
    (uint32_t) Abstract_VM_Version::vm_patch_version(),
    (uint32_t) Abstract_VM_Version::vm_build_number()
  };
  h = fnv1a(values, sizeof(values), h);
  return h;
}

static uint32_t loader_kind(InstanceKlass* holder) {
  ClassLoaderData* cld = holder->class_loader_data();
  if (cld->is_boot_class_loader_data()) {
    return MC_LOADER_BOOT;
  }
  if (cld->is_platform_class_loader_data()) {
    return MC_LOADER_PLATFORM;
  }
  if (cld->is_system_class_loader_data()) {
    return MC_LOADER_SYSTEM;
  }
  return MC_LOADER_CUSTOM;
}

static void fill_header(MCNativeCacheFileHeader* h) {
  memset(h, 0, sizeof(*h));
  h->magic = MC_NATIVE_CACHE_MAGIC;
  h->version = MC_NATIVE_CACHE_VERSION;
  h->pointer_size = (uint32_t) sizeof(void*);
  h->compressed_oops = UseCompressedOops ? 1u : 0u;
  h->compressed_class_pointers = UseCompressedClassPointers ? 1u : 0u;
  h->use_g1 = UseG1GC ? 1u : 0u;
  h->use_avx = (uint32_t) UseAVX;
  h->use_sse = (uint32_t) UseSSE;
  h->use_fma = UseFMA ? 1u : 0u;
  h->vm_major = (uint32_t) Abstract_VM_Version::vm_major_version();
  h->vm_minor = (uint32_t) Abstract_VM_Version::vm_minor_version();
  h->vm_security = (uint32_t) Abstract_VM_Version::vm_security_version();
  h->vm_patch = (uint32_t) Abstract_VM_Version::vm_patch_version();
  h->vm_build = (uint32_t) Abstract_VM_Version::vm_build_number();
  h->vm_fingerprint = vm_fingerprint();
  h->data_offset = sizeof(MCNativeCacheFileHeader);
  CodeBlob* ic_miss = CodeCache::find_blob(SharedRuntime::get_ic_miss_stub());
  h->ic_miss_stub_begin = ic_miss != nullptr ? (uint64_t) p2i(ic_miss->code_begin()) : 0;
  h->ic_miss_stub_size = ic_miss != nullptr ? (uint64_t) ic_miss->size() : 0;
}

static bool header_matches(const MCNativeCacheFileHeader* h) {
  return h->magic == MC_NATIVE_CACHE_MAGIC &&
         h->version == MC_NATIVE_CACHE_VERSION &&
         h->pointer_size == (uint32_t) sizeof(void*) &&
         h->compressed_oops == (UseCompressedOops ? 1u : 0u) &&
         h->compressed_class_pointers == (UseCompressedClassPointers ? 1u : 0u) &&
         h->use_g1 == (UseG1GC ? 1u : 0u) &&
         h->use_avx == (uint32_t) UseAVX &&
         h->use_sse == (uint32_t) UseSSE &&
         h->use_fma == (UseFMA ? 1u : 0u) &&
         h->vm_fingerprint == vm_fingerprint();
}

bool MCNativeMethodCache::mode_is(const char* name) {
  return MCNativeCacheMode != nullptr && strcmp(MCNativeCacheMode, name) == 0;
}

bool MCNativeMethodCache::mode_off() {
  return MCNativeCacheMode == nullptr || strcmp(MCNativeCacheMode, "off") == 0;
}

bool MCNativeMethodCache::should_use() {
  return MCNativeCache && !_use_disabled &&
         (mode_off() || mode_is("use") || mode_is("both") ||
          mode_is("auto") || mode_is("restore"));
}

bool MCNativeMethodCache::should_record() {
  return MCNativeCache &&
         (mode_off() || mode_is("record") || mode_is("both") || mode_is("auto"));
}

void MCNativeMethodCache::initialize() {
  if (!MCNativeCache) {
    return;
  }

  if (MCNativeCachePath == nullptr || MCNativeCachePath[0] == '\0') {
    MC_NATIVE_CACHE_LOG("MCNativeCache: no cache path configured");
    _use_disabled = true;
    return;
  }

  if (!should_use()) {
    return;
  }

  FILE* fp = os::fopen(MCNativeCachePath, "rb");
  if (fp == nullptr) {
    MC_NATIVE_CACHE_LOG("MCNativeCache: cache file not found: %s", MCNativeCachePath);
    _use_disabled = true;
    return;
  }

  MCNativeCacheFileHeader header;
  if (fread(&header, sizeof(header), 1, fp) != 1) {
    MC_NATIVE_CACHE_LOG("MCNativeCache: cannot read header from %s", MCNativeCachePath);
    fclose(fp);
    _use_disabled = true;
    return;
  }

  if (!header_matches(&header)) {
    MC_NATIVE_CACHE_LOG("MCNativeCache: cache rejected: VM/CPU/GC configuration mismatch");
    fclose(fp);
    _use_disabled = true;
    return;
  }

  _saved_ic_miss_begin = header.ic_miss_stub_begin;
  _saved_ic_miss_size = header.ic_miss_stub_size;

  uint64_t limit = (uint64_t) MCNativeCacheMaxMethods;
  uint64_t count = header.record_count;
  if (count > limit) {
    count = limit;
  }

  bool corrupt = false;
  for (uint64_t i = 0; i < count; i++) {
    MCNativeCacheRecordHeader rec;
    if (fread(&rec, sizeof(rec), 1, fp) != 1) {
      corrupt = true;
      break;
    }
    if (rec.magic != MC_NATIVE_CACHE_RECORD_MAGIC ||
        rec.version != MC_NATIVE_CACHE_VERSION) {
      corrupt = true;
      break;
    }

    char* class_name = NEW_C_HEAP_ARRAY(char, (size_t) rec.class_name_len + 1, mtInternal);
    char* method_name = NEW_C_HEAP_ARRAY(char, (size_t) rec.method_name_len + 1, mtInternal);
    char* signature = NEW_C_HEAP_ARRAY(char, (size_t) rec.signature_len + 1, mtInternal);

    bool ok = fread(class_name, 1, rec.class_name_len, fp) == rec.class_name_len &&
              fread(method_name, 1, rec.method_name_len, fp) == rec.method_name_len &&
              fread(signature, 1, rec.signature_len, fp) == rec.signature_len;
    class_name[rec.class_name_len] = '\0';
    method_name[rec.method_name_len] = '\0';
    signature[rec.signature_len] = '\0';

    bool bounds_ok =
        rec.blob_offset <= header.file_size &&
        rec.blob_size <= header.file_size - rec.blob_offset &&
        rec.oopmap_offset >= rec.blob_offset + rec.blob_size &&
        rec.oopmap_offset <= header.file_size &&
        rec.oopmap_size <= header.file_size - rec.oopmap_offset;
    if (!ok || !bounds_ok) {
      FREE_C_HEAP_ARRAY(char, class_name);
      FREE_C_HEAP_ARRAY(char, method_name);
      FREE_C_HEAP_ARRAY(char, signature);
      corrupt = true;
      break;
    }

    MCNativeCacheEntry* entry = new MCNativeCacheEntry();
    entry->_class_name = class_name;
    entry->_method_name = method_name;
    entry->_signature = signature;
    entry->_method_fingerprint = rec.method_fingerprint;
    entry->_code_size = rec.code_size;
    entry->_blob_size = rec.blob_size;
    entry->_blob_offset = rec.blob_offset;
    entry->_oopmap_size = rec.oopmap_size;
    entry->_oopmap_offset = rec.oopmap_offset;
    entry->_entry_bci = rec.entry_bci;
    entry->_comp_level = rec.comp_level;
    entry->_flags = rec.flags;
    entry->_loader_kind = rec.loader_kind;
    entry->_next = _entries;
    _entries = entry;
    _entry_count++;

    if (fseek(fp, (long) (rec.oopmap_offset + rec.oopmap_size), SEEK_SET) != 0) {
      corrupt = true;
      break;
    }
  }

  fclose(fp);

  if (corrupt) {
    MC_NATIVE_CACHE_LOG("MCNativeCache: cache index is truncated or corrupt; "
                "using %d validated entries", (int) _entry_count);
  } else {
    MC_NATIVE_CACHE_LOG("MCNativeCache: loaded %d entries from %s",
                (int) _entry_count, MCNativeCachePath);
  }
}

static void dump_one_nmethod(nmethod* nm) {
  if (_dump_file == nullptr || _dump_count >= (uint64_t) MCNativeCacheMaxMethods) {
    return;
  }

  Method* m = nm->method();
  if (m == nullptr || m->is_native()) {
    return;
  }

  ResourceMark rm;
  InstanceKlass* holder = m->method_holder();
  char* class_name = holder->name()->as_C_string();
  char* method_name = m->name()->as_C_string();
  char* signature = m->signature()->as_C_string();

  int reloc_size = nm->relocation_size();
  int oops_size = nm->oops_size();
  int metadata_size = nm->metadata_size();
  int deps_size = nm->dependencies_size();
  bool safe = reloc_size == 0 && oops_size == 0 && metadata_size == 0 && deps_size == 0;
  if (safe) {
    _safe_count++;
  } else if (MCNativeCacheVerbose && _detail_count < 8) {
    _detail_count++;
    MC_NATIVE_CACHE_LOG("MCNativeCache: complex %s::%s%s reloc=%d oops=%d meta=%d deps=%d",
                class_name, method_name, signature,
                reloc_size, oops_size, metadata_size, deps_size);
  }

  if (MCNativeCacheVerbose) {
    static int rc_logged = 0;
    if (rc_logged < 12) {
      for (RelocIterator it(nm); it.next(); ) {
        if (it.type() == relocInfo::runtime_call_type) {
          address dest = it.runtime_call_reloc()->destination();
          CodeBlob* cb = CodeCache::find_blob(dest);
          char lib[256];
          int lib_offset = -1;
          char fn[256];
          int fn_offset = -1;
          bool lok = os::dll_address_to_library_name(dest, lib, sizeof(lib), &lib_offset);
          bool fok = os::dll_address_to_function_name(dest, fn, sizeof(fn), &fn_offset, true);
          MC_NATIVE_CACHE_LOG("MCNativeCache: dump rc %s::%s%s dest=" INTPTR_FORMAT
                      " blob=%s lib=%s fn=%s",
                      class_name, method_name, signature, p2i(dest),
                      cb != nullptr ? cb->name() : "?",
                      lok ? lib : "?", fok ? fn : "?");
          rc_logged++;
          break;
        }
      }
    }
  }

  size_t class_len = strlen(class_name);
  size_t method_len = strlen(method_name);
  size_t sig_len = strlen(signature);

  MCNativeCacheRecordHeader rec;
  memset(&rec, 0, sizeof(rec));
  rec.magic = MC_NATIVE_CACHE_RECORD_MAGIC;
  rec.version = MC_NATIVE_CACHE_VERSION;
  rec.comp_level = (uint32_t) nm->comp_level();
  rec.flags = nm->is_osr_method() ? 1u : 0u;
  rec.loader_kind = loader_kind(holder);
  rec.class_name_len = (uint32_t) class_len;
  rec.method_name_len = (uint32_t) method_len;
  rec.signature_len = (uint32_t) sig_len;
  rec.method_fingerprint = m->constMethod()->fingerprint();
  rec.code_size = (uint64_t) nm->code_size();
  rec.blob_size = (uint64_t) nm->size();
  int oopmap_size = nm->oop_maps() != nullptr ? nm->oop_maps()->nr_of_bytes() : 0;
  rec.oopmap_size = (uint64_t) oopmap_size;
  rec.entry_bci = nm->is_osr_method() ? (uint64_t) nm->osr_entry_bci() : 0;

  long pos = ftell(_dump_file);
  if (pos < 0) {
    return;
  }
  rec.blob_offset = (uint64_t) pos + sizeof(rec) + class_len + method_len + sig_len;
  rec.oopmap_offset = rec.blob_offset + rec.blob_size;

  bool ok = fwrite(&rec, sizeof(rec), 1, _dump_file) == 1 &&
            fwrite(class_name, 1, class_len, _dump_file) == class_len &&
            fwrite(method_name, 1, method_len, _dump_file) == method_len &&
            fwrite(signature, 1, sig_len, _dump_file) == sig_len &&
            fwrite(nm->header_begin(), 1, (size_t) nm->size(), _dump_file) == (size_t) nm->size();

  if (ok && oopmap_size > 0) {
    ok = fwrite(nm->oop_maps(), 1, (size_t) oopmap_size, _dump_file) == (size_t) oopmap_size;
  }

  if (ok) {
    _dump_count++;
  }
}

void MCNativeMethodCache::dump_nmethod(nmethod* nm) {
  dump_one_nmethod(nm);
}

void MCNativeMethodCache::on_vm_exit() {
  if (!should_record()) {
    return;
  }
  if (MCNativeCachePath == nullptr || MCNativeCachePath[0] == '\0') {
    MC_NATIVE_CACHE_LOG("MCNativeCache: no cache path configured, nothing to record");
    return;
  }

  FILE* fp = os::fopen(MCNativeCachePath, "wb");
  if (fp == nullptr) {
    MC_NATIVE_CACHE_LOG("MCNativeCache: cannot open %s for writing", MCNativeCachePath);
    return;
  }

  _dump_file = fp;
  _dump_count = 0;
  _safe_count = 0;
  _detail_count = 0;

  MCNativeCacheFileHeader header;
  fill_header(&header);
  fwrite(&header, sizeof(header), 1, fp);

  {
    MutexLocker ml(CodeCache_lock, Mutex::_no_safepoint_check_flag);
    CodeCache::nmethods_do(dump_nmethod);
  }

  long end = ftell(fp);
  header.record_count = _dump_count;
  header.file_size = end > 0 ? (uint64_t) end : 0;
  fseek(fp, 0, SEEK_SET);
  fwrite(&header, sizeof(header), 1, fp);
  fclose(fp);
  _dump_file = nullptr;

  MC_NATIVE_CACHE_LOG("MCNativeCache: recorded %d methods (%d simple) to %s",
              (int) _dump_count, _safe_count, MCNativeCachePath);
}

nmethod* MCNativeMethodCache::try_restore(const methodHandle& method, int osr_bci, int comp_level) {
  if (!should_use() || _entry_count == 0) {
    return nullptr;
  }
  if (osr_bci != InvocationEntryBci) {
    return nullptr;
  }

  Method* m = method();
  InstanceKlass* holder = m->method_holder();
  uint32_t kind = loader_kind(holder);
  uint64_t fingerprint = m->constMethod()->fingerprint();

  ResourceMark rm;
  const char* class_name = holder->name()->as_C_string();
  const char* method_name = m->name()->as_C_string();
  const char* signature = m->signature()->as_C_string();

  for (MCNativeCacheEntry* e = _entries; e != nullptr; e = e->_next) {
    if (e->_comp_level != (uint32_t) comp_level ||
        e->_loader_kind != kind ||
        e->_method_fingerprint != fingerprint) {
      continue;
    }
    if (strcmp(e->_class_name, class_name) != 0 ||
        strcmp(e->_method_name, method_name) != 0 ||
        strcmp(e->_signature, signature) != 0) {
      continue;
    }

    MC_NATIVE_CACHE_LOG("MCNativeCache: HIT %s::%s%s level=%d code=%d blob=%d",
                class_name, method_name, signature, comp_level,
                (int) e->_code_size, (int) e->_blob_size);

    // Executing restored machine code is still experimental.  It is opt-in
    // with -XX:MCNativeCacheMode=restore so that record/use mode remains
    // safe while the inline-cache and runtime-stub relocation work is
    // completed.
    if (!mode_is("restore")) {
      MC_NATIVE_CACHE_LOG("MCNativeCache: restore disabled for %s::%s%s "
                  "(use -XX:MCNativeCacheMode=restore)",
                  class_name, method_name, signature);
      return nullptr;
    }

    if (e->_blob_size == 0 || e->_blob_size > (uint64_t) max_jint) {
      return nullptr;
    }
    if (method->adapter() == nullptr) {
      MC_NATIVE_CACHE_LOG("MCNativeCache: adapter not ready for %s::%s%s",
                  class_name, method_name, signature);
      return nullptr;
    }

    FILE* fp = os::fopen(MCNativeCachePath, "rb");
    if (fp == nullptr) {
      return nullptr;
    }
    if (fseek(fp, (long) e->_blob_offset, SEEK_SET) != 0) {
      fclose(fp);
      return nullptr;
    }

    char* blob = NEW_C_HEAP_ARRAY(char, (size_t) e->_blob_size, mtInternal);
    size_t got = fread(blob, 1, (size_t) e->_blob_size, fp);
    if (got != (size_t) e->_blob_size) {
      fclose(fp);
      FREE_C_HEAP_ARRAY(char, blob);
      return nullptr;
    }

    char* oopmap = nullptr;
    if (e->_oopmap_size > 0) {
      if (e->_oopmap_size > (uint64_t) max_jint ||
          fseek(fp, (long) e->_oopmap_offset, SEEK_SET) != 0) {
        fclose(fp);
        FREE_C_HEAP_ARRAY(char, blob);
        return nullptr;
      }
      oopmap = NEW_C_HEAP_ARRAY(char, (size_t) e->_oopmap_size, mtInternal);
      if (fread(oopmap, 1, (size_t) e->_oopmap_size, fp) != (size_t) e->_oopmap_size) {
        fclose(fp);
        FREE_C_HEAP_ARRAY(char, blob);
        FREE_C_HEAP_ARRAY(char, oopmap);
        return nullptr;
      }
    }
    fclose(fp);

    CodeBlob* cur_ic_miss = CodeCache::find_blob(SharedRuntime::get_ic_miss_stub());
    address ic_miss_begin = cur_ic_miss != nullptr
                            ? cur_ic_miss->code_begin()
                            : SharedRuntime::get_ic_miss_stub();

    int compile_id = CompileBroker::assign_compile_id(method, InvocationEntryBci);
    nmethod* nm = nmethod::new_from_cache(method, blob, (int) e->_blob_size,
                                          oopmap, (int) e->_oopmap_size,
                                          _saved_ic_miss_begin,
                                          _saved_ic_miss_size,
                                          ic_miss_begin,
                                          comp_level, InvocationEntryBci, compile_id);
    FREE_C_HEAP_ARRAY(char, blob);
    if (nm != nullptr) {
      MutexLocker ml(CompiledMethod_lock, Mutex::_no_safepoint_check_flag);
      if (nm->make_in_use()) {
        method->set_code(method, nm);
        MC_NATIVE_CACHE_LOG("MCNativeCache: RESTORED %s::%s%s level=%d",
                    class_name, method_name, signature, comp_level);
        return nm;
      }
    }

    MC_NATIVE_CACHE_LOG("MCNativeCache: restore unsupported for %s::%s%s level=%d",
                class_name, method_name, signature, comp_level);
    return nullptr;
  }

  MC_NATIVE_CACHE_LOG("MCNativeCache: MISS %s::%s%s level=%d",
              class_name, method_name, signature, comp_level);
  return nullptr;
}
