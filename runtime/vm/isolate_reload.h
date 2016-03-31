// Copyright (c) 2016, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef VM_ISOLATE_RELOAD_H_
#define VM_ISOLATE_RELOAD_H_

#include "vm/globals.h"
#include "vm/growable_array.h"
#include "vm/log.h"

DECLARE_FLAG(bool, trace_reload);

// 'Trace Isolate Reload' TIR_Print
#if defined(_MSC_VER)
#define TIR_Print(format, ...) \
    if (FLAG_trace_reload) Log::Current()->Print(format, __VA_ARGS__)
#else
#define TIR_Print(format, ...) \
    if (FLAG_trace_reload) Log::Current()->Print(format, ##__VA_ARGS__)
#endif

namespace dart {

class GrowableObjectArray;
class Isolate;
class Library;
class RawError;
class RawGrowableObjectArray;
class RawLibrary;
class RawObject;
class RawString;
class ObjectPointerVisitor;
class ObjectStore;

class IsolateReloadContext {
 public:
  IsolateReloadContext(Isolate* isolate, bool test_mode = false);
  ~IsolateReloadContext();

  void StartReload();
  void FinishReload();

  RawLibrary* saved_root_library() const;

  RawGrowableObjectArray* saved_libraries() const;

  void ReportError(const Error& error);
  void ReportSuccess();

  bool has_error() const { return has_error_; }
  RawError* error() const { return error_; }
  bool test_mode() const { return test_mode_; }

  RawClass* FindOriginalClass(const Class& cls);

 private:
  void set_saved_root_library(const Library& value);

  void set_saved_libraries(const GrowableObjectArray& value);

  void VisitObjectPointers(ObjectPointerVisitor* visitor);

  Isolate* isolate() { return isolate_; }
  ObjectStore* object_store();

  void CheckpointClassTable();
  void CommitClassTable();
  void RollbackClassTable();
  bool ValidateReload();

  // atomic_install:
  void MarkAllFunctionsForRecompilation();
  void ResetUnoptimizedICsOnStack();
  void ResetMegamorphicCaches();
  void InvalidateWorld();

  Isolate* isolate_;
  bool test_mode_;
  bool has_error_;
  intptr_t saved_num_cids_;

  RawClass* LinearFindOldClass(const Class& replacement_or_new);
  void BuildClassMapping();

  RawLibrary* LinearFindOldLibrary(const Library& replacement_or_new);
  void BuildLibraryMapping();

  void AddClassMapping(const Class& replacement_or_new,
                       const Class& original);

  void AddLibraryMapping(const Library& replacement_or_new,
                         const Library& original);

  RawClass* MappedClass(const Class& replacement_or_new);
  RawLibrary* MappedLibrary(const Library& replacement_or_new);

  RawObject** from() { return reinterpret_cast<RawObject**>(&script_uri_); }
  RawString* script_uri_;
  RawError* error_;
  RawArray* class_map_storage_;
  RawArray* library_map_storage_;
  RawLibrary* saved_root_library_;
  RawGrowableObjectArray* saved_libraries_;
  RawObject** to() { return reinterpret_cast<RawObject**>(&saved_libraries_); }

  friend class Isolate;
};

}  // namespace dart

#endif   // VM_ISOLATE_RELOAD_H_
