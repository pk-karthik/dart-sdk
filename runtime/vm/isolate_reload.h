// Copyright (c) 2016, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef VM_ISOLATE_RELOAD_H_
#define VM_ISOLATE_RELOAD_H_

#include "vm/globals.h"
#include "vm/growable_array.h"

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
  IsolateReloadContext(Isolate* isolate);
  ~IsolateReloadContext();

  RawError* StartReload();
  void FinishReload();

  RawLibrary* saved_root_library() const;

  RawGrowableObjectArray* saved_libraries() const;

 private:
  void BuildClassIdMap();
  intptr_t FindReplacementClassId(const Class& cls);

  void BuildLibraryIdMap();
  intptr_t FindReplacementLibrary(const Library& lib);

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
  intptr_t saved_num_cids_;

  struct Remapping {
    intptr_t old_id;
    intptr_t new_id;
  };

  MallocGrowableArray<Remapping> class_mappings_;
  MallocGrowableArray<Remapping> lib_mappings_;

  RawObject** from() { return reinterpret_cast<RawObject**>(&script_uri_); }
  RawString* script_uri_;
  RawLibrary* saved_root_library_;
  RawGrowableObjectArray* saved_libraries_;
  RawObject** to() { return reinterpret_cast<RawObject**>(&saved_libraries_); }

  friend class Isolate;
};

}  // namespace dart

#endif   // VM_ISOLATE_RELOAD_H_
