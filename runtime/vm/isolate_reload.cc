// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/isolate_reload.h"

#include "vm/dart_api_impl.h"
#include "vm/isolate.h"
#include "vm/object.h"
#include "vm/object_store.h"
#include "vm/safepoint.h"
#include "vm/thread.h"

namespace dart {

#define I (isolate())
#define Z (thread->zone())

IsolateReloadContext::IsolateReloadContext(Isolate* isolate)
    : isolate_(isolate),
      saved_num_cids_(-1),
      script_uri_(String::null()) {
}


IsolateReloadContext::~IsolateReloadContext() {
}


RawError* IsolateReloadContext::StartReload() {
  Thread* thread = Thread::Current();
  const Library& root_lib = Library::Handle(object_store()->root_library());
  const String& root_lib_url = String::Handle(root_lib.url());

  CheckpointClassTable();

  // Block class finalization attempts when calling into the library
  // tag handler.
  I->BlockClassFinalization();
  Object& result = Object::Handle(thread->zone());
  {
    TransitionVMToNative transition(thread);
    Api::Scope api_scope(thread);
    Dart_Handle retval =
        (I->library_tag_handler())(Dart_kScriptTag,
                                Api::NewHandle(thread, Library::null()),
                                Api::NewHandle(thread, root_lib_url.raw()));
    result = Api::UnwrapHandle(retval);
  }
  I->UnblockClassFinalization();
  if (result.IsError()) {
    return Error::Cast(result).raw();
  } else {
    return Error::null();
  }
}


void IsolateReloadContext::FinishReload() {
  BuildClassIdMap();
  fprintf(stderr, "---- DONE FINALIZING\n");
  I->class_table()->PrintNonDartClasses();

  if (ValidateReload()) {
    CommitClassTable();
  } else {
    RollbackClassTable();
  }
}


void IsolateReloadContext::CheckpointClassTable() {
  fprintf(stderr, "---- CHECKPOINTING CLASS TABLE\n");
  I->class_table()->PrintNonDartClasses();

  saved_num_cids_ = I->class_table()->NumCids();

  // Build a new libraries array which only has the dart-scheme libs.
  const GrowableObjectArray& libs =
      GrowableObjectArray::Handle(object_store()->libraries());
  const GrowableObjectArray& new_libs = GrowableObjectArray::Handle(
      GrowableObjectArray::New(Heap::kOld));
  Library& tmp_lib = Library::Handle();
  String& tmp_url = String::Handle();
  for (intptr_t i = 0; i < libs.Length(); i++) {
    tmp_lib ^= libs.At(i);
    tmp_url ^= tmp_lib.url();
    if (tmp_lib.is_dart_scheme()) {
      new_libs.Add(tmp_lib, Heap::kOld);
    }
  }
  set_saved_libraries(libs);
  object_store()->set_libraries(new_libs);

  // Reset the root library to null.
  const Library& root_lib =
      Library::Handle(object_store()->root_library());
  set_saved_root_library(root_lib);
  object_store()->set_root_library(Library::Handle());
}


void IsolateReloadContext::RollbackClassTable() {
  fprintf(stderr, "---- ROLLING BACK CLASS TABLE\n");
  Thread* thread = Thread::Current();
  ASSERT(saved_num_cids_ > 0);
  I->class_table()->SetNumCids(saved_num_cids_);
  I->class_table()->PrintNonDartClasses();

  GrowableObjectArray& saved_libs = GrowableObjectArray::Handle(
      Z, saved_libraries());
  if (!saved_libs.IsNull()) {
    object_store()->set_libraries(saved_libs);
  }
  set_saved_libraries(GrowableObjectArray::Handle());

  Library& saved_root_lib = Library::Handle(Z, saved_root_library());
  if (!saved_root_lib.IsNull()) {
    object_store()->set_root_library(saved_root_lib);
  }
  set_saved_root_library(Library::Handle());
}


void IsolateReloadContext::CommitClassTable() {
  fprintf(stderr, "---- COMMITTING CLASS TABLE\n");
  UNIMPLEMENTED();
}


bool IsolateReloadContext::ValidateReload() {
  // TODO(turnidge): Implement.
  return false;
}


intptr_t IsolateReloadContext::FindReplacementClassId(const Class& cls) {
  const intptr_t upper_cid_bound = I->class_table()->NumCids();

  const Library& lib = Library::Handle(cls.library());
  const String& url = String::Handle(lib.IsNull() ? String::null() : lib.url());
  const String& name = String::Handle(cls.Name());

  Library& new_lib = Library::Handle();
  String& new_url = String::Handle();
  String& new_name = String::Handle();
  Class& new_class = Class::Handle();

  for (intptr_t i = saved_num_cids_; i < upper_cid_bound; i++) {
    if (!I->class_table()->HasValidClassAt(i)) {
      continue;
    }
    new_class = I->class_table()->At(i);
    new_name = new_class.Name();
    if (!name.Equals(new_name)) {
      continue;
    }
    new_lib = new_class.library();
    new_url = new_lib.IsNull() ? String::null() : new_lib.url();
    if (!new_url.Equals(url)) {
      continue;
    }

    return i;
  }

  return -1;
}


void IsolateReloadContext::BuildClassIdMap() {
  const intptr_t lower_cid_bound =
      Dart::vm_isolate()->class_table()->NumCids();

  Class& cls = Class::Handle();
  for (intptr_t i = lower_cid_bound; i < saved_num_cids_; i++) {
    if (!I->class_table()->HasValidClassAt(i)) {
      continue;
    }
    cls ^= I->class_table()->At(i);
    CidMapping mapping;
    mapping.new_cid = FindReplacementClassId(cls);
    if (mapping.new_cid == -1) {
      continue;
    }
    mapping.old_cid = i;
    cid_mappings_.Add(mapping);
  }

  fprintf(stderr, "---- CLASS ID MAPPING\n");
  for (intptr_t i = 0; i < cid_mappings_.length(); i++) {
    const CidMapping& mapping = cid_mappings_[i];
    ASSERT(mapping.new_cid > 0);
    ASSERT(mapping.old_cid > 0);
    fprintf(stderr, "%" Pd " -> %" Pd "\n", mapping.new_cid, mapping.old_cid);
  }
}


RawLibrary* IsolateReloadContext::saved_root_library() const {
  return saved_root_library_;
}


void IsolateReloadContext::set_saved_root_library(const Library& value) {
  saved_root_library_ = value.raw();
}


RawGrowableObjectArray* IsolateReloadContext::saved_libraries() const {
  return saved_libraries_;
}


void IsolateReloadContext::set_saved_libraries(
    const GrowableObjectArray& value) {
  saved_libraries_ = value.raw();
}


void IsolateReloadContext::VisitObjectPointers(ObjectPointerVisitor* visitor) {
  visitor->VisitPointers(from(), to());
}


ObjectStore* IsolateReloadContext::object_store() {
  return isolate_->object_store();
}


}  // namespace dart
