// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/isolate_reload.h"

#include "vm/become.h"
#include "vm/code_generator.h"
#include "vm/compiler.h"
#include "vm/dart_api_impl.h"
#include "vm/hash_table.h"
#include "vm/isolate.h"
#include "vm/log.h"
#include "vm/object.h"
#include "vm/object_store.h"
#include "vm/safepoint.h"
#include "vm/service_event.h"
#include "vm/stack_frame.h"
#include "vm/thread.h"
#include "vm/timeline.h"
#include "vm/visitor.h"

namespace dart {

DEFINE_FLAG(bool, trace_reload, true, "Trace isolate reloading");
DEFINE_FLAG(bool, identity_reload, false, "Enable checks for identity reload.");
DEFINE_FLAG(int, reload_every, 0, "Reload every N stack overflow checks.");
DEFINE_FLAG(bool, reload_every_optimized, true, "Only from optimized code.");

#define I (isolate())
#define Z (thread->zone())

#define TIMELINE_SCOPE(name)                                                   \
    TimelineDurationScope tds##name(Thread::Current(),                         \
                                   Timeline::GetIsolateStream(),               \
                                   #name)


class ClassMapTraits {
 public:
  static bool ReportStats() { return false; }
  static const char* Name() { return "ClassMapTraits"; }

  static bool IsMatch(const Object& a, const Object& b) {
    if (!a.IsClass() || !b.IsClass()) {
      return false;
    }
    return IsolateReloadContext::IsSameClass(Class::Cast(a), Class::Cast(b));
  }

  static uword Hash(const Object& obj) {
    return String::HashRawSymbol(Class::Cast(obj).Name());
  }
};


class LibraryMapTraits {
 public:
  static bool ReportStats() { return false; }
  static const char* Name() { return "LibraryMapTraits"; }

  static bool IsMatch(const Object& a, const Object& b) {
    if (!a.IsLibrary() || !b.IsLibrary()) {
      return false;
    }
    return IsolateReloadContext::IsSameLibrary(
        Library::Cast(a), Library::Cast(b));
  }

  static uword Hash(const Object& obj) {
    return Library::Cast(obj).UrlHash();
  }
};


class BecomeMapTraits {
 public:
  static bool ReportStats() { return false; }
  static const char* Name() { return "BecomeMapTraits"; }

  static bool IsMatch(const Object& a, const Object& b) {
    return a.raw() == b.raw();
  }

  static uword Hash(const Object& obj) {
    if (obj.IsLibrary()) {
      return Library::Cast(obj).UrlHash();
    } else if (obj.IsClass()) {
      if (Class::Cast(obj).id() == kFreeListElement) {
        return 0;
      }
      return String::HashRawSymbol(Class::Cast(obj).Name());
    } else if (obj.IsField()) {
      return String::HashRawSymbol(Field::Cast(obj).name());
    }
    return 0;
  }
};


bool IsolateReloadContext::IsSameField(const Field& a, const Field& b) {
  if (a.is_static() != b.is_static()) {
    return false;
  }
  const Class& a_cls = Class::Handle(a.Owner());
  const Class& b_cls = Class::Handle(b.Owner());

  if (!IsSameClass(a_cls, b_cls)) {
    return false;
  }

  const String& a_name = String::Handle(a.name());
  const String& b_name = String::Handle(b.name());

  return a_name.Equals(b_name);
}


bool IsolateReloadContext::IsSameClass(const Class& a, const Class& b) {
  if (a.is_patch() != b.is_patch()) {
    // TODO(johnmccutchan): Should we just check the class kind bits?
    return false;
  }

  // TODO(turnidge): We need to look at generic type arguments for
  // synthetic mixin classes.  Their names are not necessarily unique
  // currently.
  const String& a_name = String::Handle(Class::Cast(a).Name());
  const String& b_name = String::Handle(Class::Cast(b).Name());

  if (!a_name.Equals(b_name)) {
    return false;
  }

  const Library& a_lib = Library::Handle(Class::Cast(a).library());
  const String& a_lib_url =
      String::Handle(a_lib.IsNull() ? String::null() : a_lib.url());

  const Library& b_lib = Library::Handle(Class::Cast(b).library());
  const String& b_lib_url =
      String::Handle(b_lib.IsNull() ? String::null() : b_lib.url());

  return a_lib_url.Equals(b_lib_url);
}


bool IsolateReloadContext::IsSameLibrary(
    const Library& a_lib, const Library& b_lib) {
  const String& a_lib_url =
      String::Handle(a_lib.IsNull() ? String::null() : a_lib.url());
  const String& b_lib_url =
      String::Handle(b_lib.IsNull() ? String::null() : b_lib.url());
  return a_lib_url.Equals(b_lib_url);
}


IsolateReloadContext::IsolateReloadContext(Isolate* isolate, bool test_mode)
    : isolate_(isolate),
      test_mode_(test_mode),
      has_error_(false),
      saved_num_cids_(-1),
      saved_class_table_(NULL),
      dead_classes_(NULL),
      num_saved_libs_(-1),
      script_uri_(String::null()),
      error_(Error::null()),
      old_classes_set_storage_(Array::null()),
      class_map_storage_(Array::null()),
      old_libraries_set_storage_(Array::null()),
      library_map_storage_(Array::null()),
      become_map_storage_(Array::null()),
      saved_root_library_(Library::null()),
      saved_libraries_(GrowableObjectArray::null()) {
  // Preallocate storage for maps.
  old_classes_set_storage_ =
      HashTables::New<UnorderedHashSet<ClassMapTraits> >(4);
  class_map_storage_ =
      HashTables::New<UnorderedHashMap<ClassMapTraits> >(4);
  old_libraries_set_storage_ =
      HashTables::New<UnorderedHashSet<LibraryMapTraits> >(4);
  library_map_storage_ =
      HashTables::New<UnorderedHashMap<LibraryMapTraits> >(4);
  become_map_storage_ =
      HashTables::New<UnorderedHashMap<BecomeMapTraits> >(4);
}


IsolateReloadContext::~IsolateReloadContext() {
}


void IsolateReloadContext::ReportError(const Error& error) {
  has_error_ = true;
  error_ = error.raw();
  if (FLAG_trace_reload) {
    THR_Print("ISO-RELOAD: Error: %s\n", error.ToErrorCString());
  }
  ServiceEvent service_event(Isolate::Current(), ServiceEvent::kIsolateReload);
  service_event.set_reload_error(&error);
  Service::HandleEvent(&service_event);
}


void IsolateReloadContext::ReportError(const String& error_msg) {
  ReportError(LanguageError::Handle(LanguageError::New(error_msg)));
}


void IsolateReloadContext::ReportSuccess() {
  ServiceEvent service_event(Isolate::Current(), ServiceEvent::kIsolateReload);
  Service::HandleEvent(&service_event);
}


void IsolateReloadContext::StartReload() {
  Thread* thread = Thread::Current();

  // Grab root library before calling CheckpointBeforeReload.
  const Library& root_lib = Library::Handle(object_store()->root_library());
  const String& root_lib_url = String::Handle(root_lib.url());

  // Switch all functions on the stack to compiled, unoptimized code.
  SwitchStackToUnoptimizedCode();

  Checkpoint();

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
    ReportError(Error::Cast(result));
  }
}


void IsolateReloadContext::FinishReload() {
  // Disable the background compiler while we are performing the reload.
  BackgroundCompiler::Disable();

  BuildClassMapping();
  BuildLibraryMapping();
  FinalizeClassTable();
  TIR_Print("---- DONE FINALIZING\n");
  if (ValidateReload()) {
    Commit();
    PostCommit();
  } else {
    Rollback();
  }

  BackgroundCompiler::Enable();
}


void IsolateReloadContext::AbortReload(const Error& error) {
  ReportError(error);
  Rollback();
}


void IsolateReloadContext::SwitchStackToUnoptimizedCode() {
  TIMELINE_SCOPE(SwitchStackToUnoptimizedCode);
  StackFrameIterator it(StackFrameIterator::kDontValidateFrames);

  Function& func = Function::Handle();
  while (it.HasNextFrame()) {
    StackFrame* frame = it.NextFrame();
    if (frame->IsDartFrame()) {
      func = frame->LookupDartFunction();
      ASSERT(!func.IsNull());
      func.EnsureHasCompiledUnoptimizedCode();
    }
  }
}


void IsolateReloadContext::CheckpointClasses() {
  TIMELINE_SCOPE(CheckpointClasses);
  TIR_Print("---- CHECKPOINTING CLASSES\n");
  // Checkpoint classes before a reload. We need to copy the following:
  // 1) The size of the class table.
  // 2) The class table itself.
  // For efficiency, we build a set of classes before the reload. This set
  // is used to pair new classes with old classes.

  ClassTable* class_table = I->class_table();

  // Copy the size of the class table.
  saved_num_cids_ = I->class_table()->NumCids();

  // Copy of the class table.
  RawClass** local_saved_class_table =
      reinterpret_cast<RawClass**>(malloc(sizeof(RawClass*) * saved_num_cids_));

  Class& cls = Class::Handle();
  UnorderedHashSet<ClassMapTraits> old_classes_set(old_classes_set_storage_);
  for (intptr_t i = 0; i < saved_num_cids_; i++) {
    if (class_table->IsValidIndex(i) &&
        class_table->HasValidClassAt(i)) {
      // Copy the class into the saved class table and add it to the set.
      local_saved_class_table[i] = class_table->At(i);
      if (i != kFreeListElement) {
        cls = class_table->At(i);
        bool already_present = old_classes_set.Insert(cls);
        ASSERT(!already_present);
      }
    } else {
      // No class at this index, mark it as NULL.
      local_saved_class_table[i] = NULL;
    }
  }
  old_classes_set_storage_ = old_classes_set.Release().raw();
  // Assigning the field must be done after saving the class table.
  saved_class_table_ = local_saved_class_table;
  TIR_Print("---- System had %" Pd " classes\n", saved_num_cids_);
}


bool IsolateReloadContext::IsCleanLibrary(const Library& lib) {
  return lib.is_dart_scheme();
}


void IsolateReloadContext::CheckpointLibraries() {
  TIMELINE_SCOPE(CheckpointLibraries);

  // Save the root library in case we abort the reload.
  const Library& root_lib =
      Library::Handle(object_store()->root_library());
  set_saved_root_library(root_lib);

  // Save the old libraries array in case we abort the reload.
  const GrowableObjectArray& libs =
      GrowableObjectArray::Handle(object_store()->libraries());
  set_saved_libraries(libs);

  // Make a filtered copy of the old libraries array. Keep "clean" libraries
  // that we will use instead of reloading.
  const GrowableObjectArray& new_libs = GrowableObjectArray::Handle(
      GrowableObjectArray::New(Heap::kOld));
  Library& lib = Library::Handle();
  UnorderedHashSet<LibraryMapTraits>
      old_libraries_set(old_libraries_set_storage_);
  num_saved_libs_ = 0;
  for (intptr_t i = 0; i < libs.Length(); i++) {
    lib ^= libs.At(i);
    if (IsCleanLibrary(lib)) {
      // We are preserving this library across the reload, assign its new index
      lib.set_index(new_libs.Length());
      new_libs.Add(lib, Heap::kOld);
      num_saved_libs_++;
    } else {
      // We are going to reload this library. Clear the index.
      lib.set_index(-1);
    }
    // Add old library to old libraries set.
    bool already_present = old_libraries_set.Insert(lib);
    ASSERT(!already_present);
  }
  old_libraries_set_storage_ = old_libraries_set.Release().raw();
  // Reset the libraries array to the filtered array.
  object_store()->set_libraries(new_libs);
  // Reset the root library to null.
  object_store()->set_root_library(Library::Handle());
}


void IsolateReloadContext::Checkpoint() {
  TIMELINE_SCOPE(Checkpoint);
  CheckpointClasses();
  CheckpointLibraries();
  // Clear the compile time constants cache.
  // TODO(turnidge): Can this be moved into Commit?
  I->object_store()->set_compile_time_constants(Object::null_array());
}


void IsolateReloadContext::RollbackClasses() {
  TIR_Print("---- ROLLING BACK CLASS TABLE\n");
#ifdef DEBUG
  VerifyInstanceClasses();
#endif
  ASSERT(saved_num_cids_ > 0);
  ASSERT(saved_class_table_ != NULL);
  ClassTable* class_table = I->class_table();
  class_table->DropNewClasses(saved_num_cids_);
  // Overwrite classes in class table with the saved classes.
  for (intptr_t i = 0; i < saved_num_cids_; i++) {
    if (class_table->IsValidIndex(i)) {
      class_table->SetAt(i, saved_class_table_[i]);
    }
  }
  free(saved_class_table_);
  saved_class_table_ = NULL;
  saved_num_cids_ = 0;
}


void IsolateReloadContext::RollbackLibraries() {
  TIR_Print("---- ROLLING BACK LIBRARY CHANGES\n");
  Thread* thread = Thread::Current();
  Library& lib = Library::Handle();
  GrowableObjectArray& saved_libs = GrowableObjectArray::Handle(
      Z, saved_libraries());
  if (!saved_libs.IsNull()) {
    object_store()->set_libraries(saved_libs);
    for (intptr_t i = 0; i < saved_libs.Length(); i++) {
      lib = Library::RawCast(saved_libs.At(i));
      // Restore indexes that were modified in CheckpointLibraries.
      lib.set_index(i);
    }
  }

  Library& saved_root_lib = Library::Handle(Z, saved_root_library());
  if (!saved_root_lib.IsNull()) {
    object_store()->set_root_library(saved_root_lib);
  }

  set_saved_root_library(Library::Handle());
  set_saved_libraries(GrowableObjectArray::Handle());
}


void IsolateReloadContext::Rollback() {
  RollbackClasses();
  RollbackLibraries();
}


#ifdef DEBUG
// ObjectVisitor which verifies that heap contains no instances with
// cid >= cid_limit.
class VerifyInstanceClassesVisitor : public ObjectVisitor {
 public:
  explicit VerifyInstanceClassesVisitor(intptr_t cid_limit)
      : ObjectVisitor(),
        cid_limit_(cid_limit) {
  }

  virtual void VisitObject(RawObject* obj) {
    if (obj->IsFreeListElement()) {
      return;
    }
    if (!obj->IsHeapObject()) {
      return;
    }
    const intptr_t cid = obj->GetClassId();
    if (cid >= cid_limit_) {
      OS::Print("Found instance above cid limit %" Pd " >= %" Pd "\n",
                cid, cid_limit_);
      UNREACHABLE();
    }
  }

 private:
  const intptr_t cid_limit_;
};


void IsolateReloadContext::VerifyInstanceClasses() {
  TIR_Print("---- BEGIN Verifying instance classes\n");
  Thread* thread = Thread::Current();
  Isolate* isolate = thread->isolate();
  Heap* heap = isolate->heap();
  VerifyInstanceClassesVisitor visitor(saved_num_cids_);
  heap->VisitObjects(&visitor);
  TIR_Print("---- DONE Verifying instance classes\n");
}


void IsolateReloadContext::VerifyMaps() {
  Class& cls = Class::Handle();
  Class& new_cls = Class::Handle();
  Class& cls2 = Class::Handle();
  Class& new_cls2 = Class::Handle();

  // Verify that two old classes aren't both mapped to the same new
  // class.  This could happen is the IsSameClass function is broken.
  UnorderedHashMap<ClassMapTraits> class_map(class_map_storage_);
  {
    UnorderedHashMap<ClassMapTraits>::Iterator it(&class_map);
    while (it.MoveNext()) {
      const intptr_t entry = it.Current();
      new_cls = Class::RawCast(class_map.GetKey(entry));
      cls = Class::RawCast(class_map.GetPayload(entry, 0));
      if (new_cls.raw() != cls.raw()) {
        UnorderedHashMap<ClassMapTraits>::Iterator it2(&class_map);
        while (it2.MoveNext()) {
          new_cls2 = Class::RawCast(class_map.GetKey(entry));
          if (new_cls.raw() == new_cls2.raw()) {
            cls2 = Class::RawCast(class_map.GetPayload(entry, 0));
            if (cls.raw() != cls2.raw()) {
              OS::PrintErr(
                  "Classes '%s' and '%s' are distinct classes but both map to "
                  "class '%s'\n",
                  cls.ToCString(), cls2.ToCString(), new_cls.ToCString());
              UNREACHABLE();
            }
          }
        }
      }
    }
  }
  class_map.Release();
}


void IsolateReloadContext::VerifyCanonicalTypeArguments() {
  Thread* thread = Thread::Current();
  const Array& table =
      Array::Handle(Z, I->object_store()->canonical_type_arguments());
  const intptr_t table_size = table.Length() - 1;
  ASSERT(Utils::IsPowerOfTwo(table_size));
  TypeArguments& element = TypeArguments::Handle(Z);
  TypeArguments& other_element = TypeArguments::Handle();
  for (intptr_t i = 0; i < table_size; i++) {
    element ^= table.At(i);
    for (intptr_t j = 0; j < table_size; j++) {
      if ((i != j) && (table.At(j) != TypeArguments::null())) {
        other_element ^= table.At(j);
        if (element.Equals(other_element)) {
          // Recursive types may be equal, but have different hashes.
          ASSERT(element.IsRecursive());
          ASSERT(other_element.IsRecursive());
          ASSERT(element.Hash() != other_element.Hash());
        }
      }
    }
  }
}
#endif


void IsolateReloadContext::RehashCanonicalTypeArguments() {
  TIMELINE_SCOPE(RehashCanonicalTypeArguments);
  Thread* thread = Thread::Current();
  // Last element of the array is the number of used elements.
  const Array& table =
      Array::Handle(Z, I->object_store()->canonical_type_arguments());
  const intptr_t table_size = table.Length() - 1;
  ASSERT(Utils::IsPowerOfTwo(table_size));
  Array& new_table = Array::Handle(Z, Array::New(table_size + 1, Heap::kOld));
  // Copy all elements from the original table to the newly allocated
  // array.
  TypeArguments& element = TypeArguments::Handle(Z);
  TypeArguments& new_element = TypeArguments::Handle(Z);
  for (intptr_t i = 0; i < table_size; i++) {
    element ^= table.At(i);
    if (!element.IsNull()) {
      const intptr_t hash = element.Hash();
      intptr_t index = hash & (table_size - 1);
      new_element ^= new_table.At(index);
      while (!new_element.IsNull()) {
        if (new_element.Equals(element)) {
          // When we replace old classes with new classes, we can
          // sometimes produce duplication type arguments.
          //
          // TODO(turnidge): Talk to Regis about this case.
          break;
        }
        index = (index + 1) & (table_size - 1);  // Move to next element.
        new_element ^= new_table.At(index);
      }
      new_table.SetAt(index, element);
    }
  }
  // Copy used count.
  const Object& used_count = Object::Handle(Z, table.At(table_size));
  new_table.SetAt(table_size, used_count);
  // Remember the new table now.
  I->object_store()->set_canonical_type_arguments(new_table);
#ifdef DEBUG
  VerifyCanonicalTypeArguments();
#endif
}


void IsolateReloadContext::Commit() {
  TIMELINE_SCOPE(Commit);
  // I->class_table()->PrintNonDartClasses();
  TIR_Print("---- COMMITTING REVERSE MAP\n");

#ifdef DEBUG
  VerifyMaps();
  VerifyInstanceClasses();
#endif

  {
    TIMELINE_SCOPE(CopyStaticFieldsAndPatchFieldsAndFunctions);
    // Copy static field values from the old classes to the new classes.
    // Patch fields and functions in the old classes so that they retain
    // the old script.
    Class& cls = Class::Handle();
    Class& new_cls = Class::Handle();

    UnorderedHashMap<ClassMapTraits> class_map(class_map_storage_);

    {
      UnorderedHashMap<ClassMapTraits>::Iterator it(&class_map);
      while (it.MoveNext()) {
        const intptr_t entry = it.Current();
        new_cls = Class::RawCast(class_map.GetKey(entry));
        cls = Class::RawCast(class_map.GetPayload(entry, 0));
        if (new_cls.raw() != cls.raw()) {
          ASSERT(new_cls.is_enum_class() == cls.is_enum_class());
          if (new_cls.is_enum_class() && new_cls.is_finalized()) {
            new_cls.ReplaceEnum(cls);
          }
          new_cls.CopyStaticFieldValues(cls);
          new_cls.CopyCanonicalConstants(cls);
          cls.PatchFieldsAndFunctions();
        }
      }
    }

    class_map.Release();
  }

  // Copy over certain properties of libraries, e.g. is the library
  // debuggable?
  {
    TIMELINE_SCOPE(CopyLibraryBits);
    Library& lib = Library::Handle();
    Library& new_lib = Library::Handle();

    UnorderedHashMap<LibraryMapTraits> lib_map(library_map_storage_);

    {
      // Reload existing libraries.
      UnorderedHashMap<LibraryMapTraits>::Iterator it(&lib_map);

      while (it.MoveNext()) {
        const intptr_t entry = it.Current();
        ASSERT(entry != -1);
        new_lib = Library::RawCast(lib_map.GetKey(entry));
        lib = Library::RawCast(lib_map.GetPayload(entry, 0));
        new_lib.set_debuggable(lib.IsDebuggable());
      }
    }

    // Release the library map.
    lib_map.Release();
  }

  {
    TIMELINE_SCOPE(UpdateLibrariesArray);
    // Update the libraries array.
    Library& lib = Library::Handle();
    const GrowableObjectArray& libs = GrowableObjectArray::Handle(
        I->object_store()->libraries());
    for (intptr_t i = 0; i < libs.Length(); i++) {
      lib = Library::RawCast(libs.At(i));
      TIR_Print("Lib '%s' at index %" Pd "\n", lib.ToCString(), i);
      lib.set_index(i);
    }

    // Initialize library side table.
    library_infos_.SetLength(libs.Length());
    for (intptr_t i = 0; i < libs.Length(); i++) {
      lib = Library::RawCast(libs.At(i));
      // Mark the library dirty if it comes after the libraries we saved.
      library_infos_[i].dirty = i >= num_saved_libs_;
    }
  }

  {
    UnorderedHashMap<BecomeMapTraits> become_map(become_map_storage_);
    intptr_t replacement_count = become_map.NumOccupied();
    const Array& before =
        Array::Handle(Array::New(replacement_count, Heap::kOld));
    const Array& after =
        Array::Handle(Array::New(replacement_count, Heap::kOld));
    Object& obj = Object::Handle();
    intptr_t replacement_index = 0;
    UnorderedHashMap<BecomeMapTraits>::Iterator it(&become_map);
    while (it.MoveNext()) {
      const intptr_t entry = it.Current();
      obj = become_map.GetKey(entry);
      before.SetAt(replacement_index, obj);
      obj = become_map.GetPayload(entry, 0);
      after.SetAt(replacement_index, obj);
      replacement_index++;
    }
    ASSERT(replacement_index == replacement_count);
    become_map.Release();

    Become::ElementsForwardIdentity(before, after);
  }

  if (FLAG_identity_reload) {
    if (saved_num_cids_ != I->class_table()->NumCids()) {
      TIR_Print("Identity reload failed! B#C=%" Pd " A#C=%" Pd "\n",
                saved_num_cids_,
                I->class_table()->NumCids());
    }
    const GrowableObjectArray& saved_libs =
        GrowableObjectArray::Handle(saved_libraries());
    const GrowableObjectArray& libs =
        GrowableObjectArray::Handle(I->object_store()->libraries());
    if (saved_libs.Length() != libs.Length()) {
     TIR_Print("Identity reload failed! B#L=%" Pd " A#L=%" Pd "\n",
               saved_libs.Length(),
               libs.Length());
    }
  }

  // The canonical types were hashed based on the old class ids.  Rehash.
  RehashCanonicalTypeArguments();
}


bool IsolateReloadContext::IsDirty(const Library& lib) {
  const intptr_t index = lib.index();
  if (index == static_cast<classid_t>(-1)) {
    // Treat deleted libraries as dirty.
    return true;
  }
  ASSERT((index >= 0) && (index < library_infos_.length()));
  return library_infos_[index].dirty;
}


void IsolateReloadContext::PostCommit() {
  TIMELINE_SCOPE(PostCommit);
  set_saved_root_library(Library::Handle());
  set_saved_libraries(GrowableObjectArray::Handle());
  InvalidateWorld();
}


bool IsolateReloadContext::ValidateReload() {
  TIMELINE_SCOPE(ValidateReload);
  if (has_error_) {
    return false;
  }

  // Already built.
  ASSERT(class_map_storage_ != Array::null());
  UnorderedHashMap<ClassMapTraits> map(class_map_storage_);
  UnorderedHashMap<ClassMapTraits>::Iterator it(&map);
  Class& cls = Class::Handle();
  Class& new_cls = Class::Handle();
  while (it.MoveNext()) {
    const intptr_t entry = it.Current();
    new_cls = Class::RawCast(map.GetKey(entry));
    cls = Class::RawCast(map.GetPayload(entry, 0));
    if (new_cls.raw() != cls.raw()) {
      if (!cls.CanReload(new_cls)) {
        map.Release();
        return false;
      }
    }
  }
  map.Release();
  return true;
}


RawClass* IsolateReloadContext::FindOriginalClass(const Class& cls) {
  return MappedClass(cls);
}


RawClass* IsolateReloadContext::GetClassForHeapWalkAt(intptr_t cid) {
  if (saved_class_table_ != NULL) {
    ASSERT(cid > 0);
    ASSERT(cid < saved_num_cids_);
    return saved_class_table_[cid];
  } else {
    return isolate_->class_table()->At(cid);
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
  if (saved_class_table_ != NULL) {
    visitor->VisitPointers(
        reinterpret_cast<RawObject**>(&saved_class_table_[0]), saved_num_cids_);
  }
}


ObjectStore* IsolateReloadContext::object_store() {
  return isolate_->object_store();
}


static void ResetICs(const Function& function, const Code& code) {
  if (function.ic_data_array() == Array::null()) {
    return;  // Already reset in an earlier round.
  }

  Thread* thread = Thread::Current();
  Zone* zone = thread->zone();

  ZoneGrowableArray<const ICData*>* ic_data_array =
      new(zone) ZoneGrowableArray<const ICData*>();
  function.RestoreICDataMap(ic_data_array, false /* clone ic-data */);
  if (ic_data_array->length() == 0) {
    return;
  }
  const PcDescriptors& descriptors =
      PcDescriptors::Handle(code.pc_descriptors());
  PcDescriptors::Iterator iter(descriptors, RawPcDescriptors::kIcCall |
                                            RawPcDescriptors::kUnoptStaticCall);
  while (iter.MoveNext()) {
    const ICData* ic_data = (*ic_data_array)[iter.DeoptId()];
    if (ic_data == NULL) {
      continue;
    }
    bool is_static_call = iter.Kind() == RawPcDescriptors::kUnoptStaticCall;
    ic_data->Reset(is_static_call);
  }
}


void IsolateReloadContext::ResetUnoptimizedICsOnStack() {
  Code& code = Code::Handle();
  Function& function = Function::Handle();
  ObjectPool& object_table = ObjectPool::Handle();
  Object& object_table_entry = Object::Handle();
  DartFrameIterator iterator;
  StackFrame* frame = iterator.NextFrame();
  while (frame != NULL) {
    code = frame->LookupDartCode();
    if (code.is_optimized()) {
      // If this code is optimized, we need to reset the ICs in the
      // corresponding unoptimized code, which will be executed when the stack
      // unwinds to the the optimized code. We must use the unoptimized code
      // referenced from the optimized code's deopt object table, because this
      // is the code that will be used to finish the activation after deopt. It
      // can be different from the function's current unoptimized code, which
      // may be null if we've already done an atomic install or different code
      // if the function has already been recompiled.
      function = code.function();
      object_table = code.object_pool();
      intptr_t reset_count = 0;
      for (intptr_t i = 0; i < object_table.Length(); i++) {
        if (object_table.InfoAt(i) != ObjectPool::kTaggedObject) {
          continue;
        }
        object_table_entry = object_table.ObjectAt(i);
        if (object_table_entry.IsCode()) {
          code ^= object_table_entry.raw();
          if (code.function() == function.raw()) {
            reset_count++;
            ResetICs(function, code);
          }
          // Why are other code objects in this table? Allocation stubs?
        }
      }
      // ASSERT(reset_count == 1);
      // vm shot itself in the foot: no reference to unopt code.
    } else {
      function = code.function();
      ResetICs(function, code);
    }
    frame = iterator.NextFrame();
  }
}


void IsolateReloadContext::ResetMegamorphicCaches() {
  object_store()->set_megamorphic_cache_table(GrowableObjectArray::Handle());
  // Since any current optimized code will not make any more calls, it may be
  // better to clear the table instead of clearing each of the caches, allow
  // the current megamorphic caches get GC'd and any new optimized code allocate
  // new ones.
}


class MarkFunctionsForRecompilation : public ObjectVisitor {
 public:
  MarkFunctionsForRecompilation(Isolate* isolate,
                                IsolateReloadContext* reload_context)
    : ObjectVisitor(),
      handle_(Object::Handle()),
      owning_class_(Class::Handle()),
      owning_lib_(Library::Handle()),
      code_(Code::Handle()),
      reload_context_(reload_context) {
  }

  virtual void VisitObject(RawObject* obj) {
    // Free-list elements cannot even be wrapped in handles.
    if (obj->IsFreeListElement()) {
      return;
    }
    handle_ = obj;
    if (handle_.IsFunction()) {
      const Function& func = Function::Cast(handle_);

      // Switch to unoptimized code or the lazy compilation stub.
      func.SwitchToLazyCompiledUnoptimizedCode();

      // Grab the current code.
      code_ = func.CurrentCode();
      ASSERT(!code_.IsNull());
      const bool clear_code = IsFromDirtyLibrary(func);
      const bool stub_code = code_.IsStubCode();

      // Zero edge counters.
      func.ZeroEdgeCounters();

      if (!stub_code) {
        if (clear_code) {
          ClearAllCode(func);
        } else {
          PreserveUnoptimizedCode(func);
        }
      }

      // Clear counters.
      func.set_usage_counter(0);
      func.set_deoptimization_counter(0);
      func.set_optimized_instruction_count(0);
      func.set_optimized_call_site_count(0);
    }
  }

 private:
  void ClearAllCode(const Function& func) {
    // Null out the ICData array and code.
    func.ClearICDataArray();
    func.ClearCode();
  }

  void PreserveUnoptimizedCode(const Function& func) {
    ASSERT(!code_.IsNull());
    // We are preserving the unoptimized code, fill all ICData arrays with
    // the sentinel values so that we have no stale type feedback.
    func.FillICDataWithSentinels(code_);
  }

  bool IsFromDirtyLibrary(const Function& func) {
    owning_class_ = func.Owner();
    owning_lib_ = owning_class_.library();
    return reload_context_->IsDirty(owning_lib_);
  }

  Object& handle_;
  Class& owning_class_;
  Library& owning_lib_;
  Code& code_;
  IsolateReloadContext* reload_context_;
};


void IsolateReloadContext::MarkAllFunctionsForRecompilation() {
  TIMELINE_SCOPE(MarkAllFunctionsForRecompilation);
  MarkFunctionsForRecompilation visitor(isolate_, this);
  isolate_->heap()->VisitObjects(&visitor);
}


void IsolateReloadContext::InvalidateWorld() {
  ResetMegamorphicCaches();

  DeoptimizeFunctionsOnStack();

  {
    NoSafepointScope no_safepoint;
    HeapIterationScope heap_iteration_scope;

    ResetUnoptimizedICsOnStack();
    MarkAllFunctionsForRecompilation();
  }
}


RawClass* IsolateReloadContext::MappedClass(const Class& replacement_or_new) {
  UnorderedHashMap<ClassMapTraits> map(class_map_storage_);
  Class& cls = Class::Handle();
  cls ^= map.GetOrNull(replacement_or_new);
  // No need to update storage address because no mutation occurred.
  map.Release();
  return cls.raw();
}


RawLibrary* IsolateReloadContext::MappedLibrary(
    const Library& replacement_or_new) {
  return Library::null();
}


RawClass* IsolateReloadContext::OldClassOrNull(
    const Class& replacement_or_new) {
  UnorderedHashSet<ClassMapTraits> old_classes_set(old_classes_set_storage_);
  Class& cls = Class::Handle();
  cls ^= old_classes_set.GetOrNull(replacement_or_new);
  old_classes_set_storage_ = old_classes_set.Release().raw();
  return cls.raw();
}


void IsolateReloadContext::BuildClassMapping() {
  const intptr_t lower_cid_bound = saved_num_cids_;
  const intptr_t upper_cid_bound = I->class_table()->NumCids();
  ClassTable* class_table = I->class_table();
  Class& replacement_or_new = Class::Handle();
  Class& old = Class::Handle();
  for (intptr_t i = lower_cid_bound; i < upper_cid_bound; i++) {
    if (!class_table->HasValidClassAt(i)) {
      continue;
    }
    replacement_or_new = class_table->At(i);
    old ^= OldClassOrNull(replacement_or_new);
    if (old.IsNull()) {
      if (FLAG_identity_reload) {
        TIR_Print("Could not find replacement class for %s\n",
                  replacement_or_new.ToCString());
        UNREACHABLE();
      }
      // New class.
      AddClassMapping(replacement_or_new, replacement_or_new);
    } else {
      // Replaced class.
      AddClassMapping(replacement_or_new, old);
    }
  }
}


bool IsolateReloadContext::IsDeadClassAt(intptr_t index) {
  ASSERT(dead_classes_ != NULL);
  return dead_classes_->At(index);
}


void IsolateReloadContext::MarkClassDeadAt(intptr_t index) {
  ASSERT(dead_classes_ != NULL);
  (*dead_classes_)[index] = true;
}


void IsolateReloadContext::CompactClassTable() {
  const intptr_t top = I->class_table()->NumCids();
  intptr_t new_top = saved_num_cids_;
  for (intptr_t free_index = saved_num_cids_; free_index < top; free_index++) {
    // Scan forward until we find a cleared class.
    if (!IsDeadClassAt(free_index)) {
      new_top++;
      continue;
    }

    for (intptr_t cls_index = free_index + 1; cls_index < top; cls_index++) {
      // Scan forward until we find a live class.
      if (IsDeadClassAt(cls_index)) {
        continue;
      }
      // Move the class into the free slot.
      I->class_table()->MoveClass(free_index, cls_index);
      // Mark |cls_index| as dead.
      MarkClassDeadAt(cls_index);
      new_top++;
      break;
    }
  }

  I->class_table()->DropNewClasses(new_top);
}


void IsolateReloadContext::FinalizeClassTable() {
  // Finalize the class table so that it looks like the class table will when
  // the reload succeeds. We may still abort the reload, but we need to finalize
  // the class table before proceeding further.
  TIMELINE_SCOPE(FinalizeClassTable);

  ASSERT(dead_classes_ == NULL);
  // Initialize the dead classes array.
  dead_classes_ = new MallocGrowableArray<bool>();
  dead_classes_->SetLength(I->class_table()->NumCids());;
  for (intptr_t i = 0; i < dead_classes_->length(); i++) {
    (*dead_classes_)[i] = false;
  }

  // Move classes in the class table and update their cid.
  Class& cls = Class::Handle();
  Class& new_cls = Class::Handle();

  UnorderedHashMap<ClassMapTraits> class_map(class_map_storage_);

  UnorderedHashMap<ClassMapTraits>::Iterator it(&class_map);
  while (it.MoveNext()) {
    const intptr_t entry = it.Current();
    new_cls = Class::RawCast(class_map.GetKey(entry));
    cls = Class::RawCast(class_map.GetPayload(entry, 0));
    if (new_cls.raw() != cls.raw()) {
      TIR_Print("Replaced '%s'@%" Pd " with '%s'@%" Pd "\n",
                cls.ToCString(), cls.id(),
                new_cls.ToCString(), new_cls.id());
      // Replace |cls| with |new_cls| in the class table.
      ASSERT(!IsDeadClassAt(new_cls.id()));
      MarkClassDeadAt(new_cls.id());
      // TODO(rmacnak): Should be handled by the become forward.
      I->class_table()->ReplaceClass(cls, new_cls);
      AddBecomeMapping(cls, new_cls);
    }
  }

  class_map.Release();

  TIR_Print("---- Compacting the class table\n");
  CompactClassTable();
  TIR_Print("---- System has %" Pd " classes\n", I->class_table()->NumCids());

  delete dead_classes_;
  dead_classes_ = NULL;
}


RawLibrary* IsolateReloadContext::OldLibraryOrNull(
    const Library& replacement_or_new) {
  UnorderedHashSet<LibraryMapTraits>
      old_libraries_set(old_libraries_set_storage_);
  Library& lib = Library::Handle();
  lib ^= old_libraries_set.GetOrNull(replacement_or_new);
  old_libraries_set_storage_ = old_libraries_set.Release().raw();
  return lib.raw();
}


void IsolateReloadContext::BuildLibraryMapping() {
  const GrowableObjectArray& libs =
      GrowableObjectArray::Handle(object_store()->libraries());

  Library& replacement_or_new = Library::Handle();
  Library& old = Library::Handle();
  for (intptr_t i = 0; i < libs.Length(); i++) {
    replacement_or_new = Library::RawCast(libs.At(i));
    if (IsCleanLibrary(replacement_or_new)) {
      continue;
    }
    old ^= OldLibraryOrNull(replacement_or_new);
    if (old.IsNull()) {
      // New library.
      AddLibraryMapping(replacement_or_new, replacement_or_new);
    } else {
      ASSERT(!replacement_or_new.is_dart_scheme());
      // Replaced class.
      AddLibraryMapping(replacement_or_new, old);

      AddBecomeMapping(old, replacement_or_new);
    }
  }
}


void IsolateReloadContext::AddClassMapping(const Class& replacement_or_new,
                                           const Class& original) {
  UnorderedHashMap<ClassMapTraits> map(class_map_storage_);
  bool update = map.UpdateOrInsert(replacement_or_new, original);
  ASSERT(!update);
  // The storage given to the map may have been reallocated, remember the new
  // address.
  class_map_storage_ = map.Release().raw();
}


void IsolateReloadContext::AddLibraryMapping(const Library& replacement_or_new,
                                             const Library& original) {
  UnorderedHashMap<LibraryMapTraits> map(library_map_storage_);
  bool update = map.UpdateOrInsert(replacement_or_new, original);
  ASSERT(!update);
  // The storage given to the map may have been reallocated, remember the new
  // address.
  library_map_storage_ = map.Release().raw();
}


void IsolateReloadContext::AddStaticFieldMapping(
    const Field& old_field, const Field& new_field) {
  ASSERT(old_field.is_static());
  ASSERT(new_field.is_static());

  AddBecomeMapping(old_field, new_field);
}


void IsolateReloadContext::AddBecomeMapping(const Object& old,
                                            const Object& neu) {
  ASSERT(become_map_storage_ != Array::null());
  UnorderedHashMap<BecomeMapTraits> become_map(become_map_storage_);
  bool update = become_map.UpdateOrInsert(old, neu);
  ASSERT(!update);
  become_map_storage_ = become_map.Release().raw();
}

}  // namespace dart
