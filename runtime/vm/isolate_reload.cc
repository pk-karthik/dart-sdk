// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/isolate_reload.h"

#include "vm/code_generator.h"
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
#include "vm/visitor.h"

namespace dart {

DEFINE_FLAG(bool, trace_reload, true, "Trace isolate reloading");

#define I (isolate())
#define Z (thread->zone())


class ClassMapTraits {
 public:
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


class ReverseMapTraits {
 public:
  static bool IsMatch(const Object& a, const Object& b) {
    if (a.IsLibrary() && b.IsLibrary()) {
      return IsolateReloadContext::IsSameLibrary(
          Library::Cast(a), Library::Cast(b));
    } else if (a.IsClass() && b.IsClass()) {
      if (Class::Cast(a).id() == kFreeListElement) {
        return false;
      }
      if (Class::Cast(b).id() == kFreeListElement) {
        return false;
      }
      return IsolateReloadContext::IsSameClass(Class::Cast(a), Class::Cast(b));
    } else {
      return false;
    }
  }

  static uword Hash(const Object& obj) {
    if (obj.IsLibrary()) {
      return Library::Cast(obj).UrlHash();
    } else if (obj.IsClass()) {
      if (Class::Cast(obj).id() == kFreeListElement) {
        return 0;
      }
      return String::HashRawSymbol(Class::Cast(obj).Name());
    } else {
      return 0;
    }
  }
};


class UpdateClassesVisitor : public ObjectPointerVisitor {
 public:
  UpdateClassesVisitor(Isolate* isolate)
      : ObjectPointerVisitor(isolate),
        key_(Object::Handle()),
        value_(Object::Handle()),
        reverse_map_storage_(Array::Handle(
            isolate->reload_context()->reverse_map_storage_)),
        class_map_storage_(Array::Handle(
            isolate->reload_context()->class_map_storage_)),
        library_map_storage_(Array::Handle(
            isolate->reload_context()->library_map_storage_)),
        context_(isolate->reload_context()),
        replacement_count_(0) {
  }

  virtual void VisitPointers(RawObject** first, RawObject** last) {
    if (IsContainedInIsolateReloadContext(first)) {
      return;
    }
    UnorderedHashMap<ReverseMapTraits> reverse_map(reverse_map_storage_.raw());
    for (RawObject** p = first; p <= last; p++) {
      if (!(*p)->IsHeapObject()) {
        continue;
      }
      if (!(*p)->IsReplacedObject()) {
        continue;
      }
      key_ = *p;
      const intptr_t entry = reverse_map.FindKey(key_);
      ASSERT(entry != -1);
      value_ = reverse_map.GetPayload(entry, 0);
      if (key_.raw() == value_.raw()) {
        continue;
      }
      *p = value_.raw();
      replacement_count_++;
    }

    reverse_map.Release();
  }

  intptr_t replacement_count() const { return replacement_count_; }

 private:
  bool IsContainedInIsolateReloadContext(RawObject** first) {
    if (reverse_map_storage_.Contains(first)) {
      return true;
    }
    if (class_map_storage_.Contains(first)) {
      return true;
    }
    if (library_map_storage_.Contains(first)) {
      return true;
    }
    return false;
  }

  Object& key_;
  Object& value_;
  Array& reverse_map_storage_;
  Array& class_map_storage_;
  Array& library_map_storage_;
  IsolateReloadContext* context_;
  intptr_t replacement_count_;
};


bool IsolateReloadContext::IsSameClass(const Class& a, const Class& b) {
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
      num_saved_libs_(-1),
      script_uri_(String::null()),
      error_(Error::null()),
      class_map_storage_(Array::null()),
      library_map_storage_(Array::null()),
      reverse_map_storage_(Array::null()),
      saved_root_library_(Library::null()),
      saved_libraries_(GrowableObjectArray::null()) {
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

  CheckpointBeforeReload();

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
  reverse_map_storage_ =
      HashTables::New<UnorderedHashMap<ReverseMapTraits> >(4);
  BuildClassMapping();
  BuildLibraryMapping();
  TIR_Print("---- DONE FINALIZING\n");
  I->class_table()->PrintNonDartClasses();

  if (ValidateReload()) {
    if (true) {
      CommitReverseMap();
    } else {
      CommitClassTable();
    }
    PostCommit();
  } else {
    Rollback();
  }
}


void IsolateReloadContext::SwitchStackToUnoptimizedCode() {
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
  TIR_Print("---- CHECKPOINTING CLASSES\n");
  I->class_table()->PrintNonDartClasses();
  saved_num_cids_ = I->class_table()->NumCids();
}


void IsolateReloadContext::CheckpointLibraries() {
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
  num_saved_libs_ = new_libs.Length();

  // Reset the root library to null.
  const Library& root_lib =
      Library::Handle(object_store()->root_library());
  set_saved_root_library(root_lib);
  object_store()->set_root_library(Library::Handle());
}


void IsolateReloadContext::CheckpointBeforeReload() {
  CheckpointClasses();
  CheckpointLibraries();
  // Clear the compile time constants cache.
  // TODO(turnidge): Can this be moved into Commit?
  I->object_store()->set_compile_time_constants(Object::null_array());
}


void IsolateReloadContext::Rollback() {
  TIR_Print("---- ROLLING BACK CLASS TABLE\n");
  Thread* thread = Thread::Current();
  ASSERT(saved_num_cids_ > 0);
  I->class_table()->DropNewClasses(saved_num_cids_);
  I->class_table()->PrintNonDartClasses();

  TIR_Print("---- ROLLING BACK LIBRARY CHANGES\n");
  GrowableObjectArray& saved_libs = GrowableObjectArray::Handle(
      Z, saved_libraries());
  if (!saved_libs.IsNull()) {
    object_store()->set_libraries(saved_libs);
  }

  Library& saved_root_lib = Library::Handle(Z, saved_root_library());
  if (!saved_root_lib.IsNull()) {
    object_store()->set_root_library(saved_root_lib);
  }

  set_saved_root_library(Library::Handle());
  set_saved_libraries(GrowableObjectArray::Handle());
}


void IsolateReloadContext::CommitReverseMap() {
  Thread* thread = Thread::Current();
  TIR_Print("---- COMMITTING REVERSE MAP\n");

  {
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
          new_cls.CopyStaticFieldValues(cls);
          cls.PatchFieldsAndFunctions();
        }
      }
    }

    class_map.Release();
  }

  {
    // Move classes in the class table and update their cid.
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
          TIR_Print("Replaced '%s'@%" Pd " with '%s'@%" Pd "\n",
                    cls.ToCString(), cls.id(),
                    new_cls.ToCString(), new_cls.id());
          // Replace |cls| with |new_cls| in the class table.
          I->class_table()->ReplaceClass(cls, new_cls);
        }
      }
    }

    class_map.Release();
  }

  // Copy over certain properties of libraries, e.g. is the library
  // debuggable?
  {
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
    HeapIterationScope heap_iteration_scope;
    Isolate* isolate = thread->isolate();
    UpdateClassesVisitor ucv(isolate);
    // isolate->IterateObjectPointers(&ucv, true);
    TIR_Print("---- Scanning heap\n");
    isolate->heap()->WriteProtectCode(false);
    isolate->heap()->VisitObjectPointers(&ucv);
    isolate->heap()->WriteProtectCode(true);
    TIR_Print("---- Scanning object store\n");
    isolate->object_store()->VisitObjectPointers(&ucv);
    TIR_Print("---- Scanning stub code\n");
    StubCode::VisitObjectPointers(&ucv);
    TIR_Print("---- Performed %" Pd " replacements\n", ucv.replacement_count());
  }

  TIR_Print("---- Compacting the class table\n");
  I->class_table()->CompactNewClasses(saved_num_cids_);
}


bool IsolateReloadContext::IsDirty(const Library& lib) {
  const intptr_t index = lib.index();
  ASSERT((index >= 0) && (index < library_infos_.length()));
  return library_infos_[index].dirty;
}


void IsolateReloadContext::CommitClassTable() {
  Thread* thread = Thread::Current();
  TIR_Print("---- COMMITTING CLASS TABLE\n");

  {
    Class& cls = Class::Handle();
    Class& new_cls = Class::Handle();

    UnorderedHashMap<ClassMapTraits> class_map(class_map_storage_);

    {
      // Reload existing classes.
      UnorderedHashMap<ClassMapTraits>::Iterator it(&class_map);
      while (it.MoveNext()) {
        const intptr_t entry = it.Current();
        new_cls = Class::RawCast(class_map.GetKey(entry));
        cls = Class::RawCast(class_map.GetPayload(entry, 0));
        if (new_cls.raw() != cls.raw()) {
          cls.Reload(new_cls);
        }
      }
    }

    {
      // Remove unneeded classes from the class table.
      UnorderedHashMap<ClassMapTraits>::Iterator it(&class_map);
      while (it.MoveNext()) {
        const intptr_t entry = it.Current();
        ASSERT(entry != -1);
        new_cls = Class::RawCast(class_map.GetKey(entry));
        cls = Class::RawCast(class_map.GetPayload(entry, 0));
        if (new_cls.raw() != cls.raw()) {
          I->class_table()->ClearClassAt(new_cls.id());
        }
      }
    }

    // Release the class map.
    class_map.Release();
  }


  {
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
        if (new_lib.raw() != lib.raw()) {
          lib.Reload(new_lib);
        }
      }
    }

    // Release the library map.
    lib_map.Release();
  }

  I->class_table()->CompactNewClasses(saved_num_cids_);

  // NO TWO.
  GrowableObjectArray& libs = GrowableObjectArray::Handle(
      Z, saved_libraries());
  if (!libs.IsNull()) {
    object_store()->set_libraries(libs);
  }

  Library& saved_root_lib = Library::Handle(Z, saved_root_library());
  if (!saved_root_lib.IsNull()) {
    object_store()->set_root_library(saved_root_lib);
  }
}


void IsolateReloadContext::PostCommit() {
  ClearReplacedObjectBits();
  set_saved_root_library(Library::Handle());
  set_saved_libraries(GrowableObjectArray::Handle());
  InvalidateWorld();
}


void IsolateReloadContext::ClearReplacedObjectBits() {
  UnorderedHashMap<ReverseMapTraits> reverse_map(reverse_map_storage_);
  UnorderedHashMap<ReverseMapTraits>::Iterator it(&reverse_map);

  Object& obj = Object::Handle();
  while (it.MoveNext()) {
    const intptr_t entry = it.Current();
    obj = reverse_map.GetKey(entry);
    obj.raw()->ClearIsReplacedObject();
  }

  reverse_map.Release();
}


bool IsolateReloadContext::ValidateReload() {
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


static void ResetICs(const Function& function, const Code& code) {
  if (function.ic_data_array() == Array::null()) {
    return;  // Already reset in an earlier round.
  }

  Thread* thread = Thread::Current();
  Zone* zone = thread->zone();

  ZoneGrowableArray<const ICData*>* ic_data_array =
      new(zone) ZoneGrowableArray<const ICData*>();
  function.RestoreICDataMap(ic_data_array, false /* clone ic-data */);
  const PcDescriptors& descriptors =
      PcDescriptors::Handle(code.pc_descriptors());
  PcDescriptors::Iterator iter(descriptors, RawPcDescriptors::kIcCall |
                                            RawPcDescriptors::kUnoptStaticCall);
  while (iter.MoveNext()) {
    const ICData* ic_data = (*ic_data_array)[iter.DeoptId()];
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
  explicit MarkFunctionsForRecompilation(Isolate* isolate)
    : ObjectVisitor(isolate),
      handle_(Object::Handle()),
      owning_class_(Class::Handle()),
      owning_lib_(Library::Handle()),
      code_(Code::Handle()) {
  }

  virtual void VisitObject(RawObject* obj) {
    // Free-list elements cannot even be wrapped in handles.
    if (obj->IsFreeListElement()) {
      return;
    }
    handle_ = obj;
    if (handle_.IsFunction()) {
      const Function& func = Function::Cast(handle_);

      if (IsDartSchemeFunction(func)) {
        // TODO(johnmccutchan): Determine how to keep dart: code alive.
      }

      // Replace the instructions of most functions with the compilation stub so
      // unqualified invocations will be recompiled to the correct kind. But
      // leave the stub to patch the function for extracted properties so they
      // will still be patched if more than one modification happens before they
      // are next called.
      code_ = func.CurrentCode();
      if (!code_.IsStubCode()) {
        func.ClearICDataArray();  // Don't reuse IC data in next compilation.
        func.ClearCode();

        // Type feedback data is gone, don't trigger optimization again too
        // soon.
        func.set_usage_counter(0);
        func.set_deoptimization_counter(0);
      }
    }
  }

 private:
  bool IsDartSchemeFunction(const Function& func) {
    ASSERT(!func.IsNull());
    owning_class_ = func.Owner();
    owning_lib_ = owning_class_.library();
    return owning_lib_.is_dart_scheme();
  }

  Object& handle_;
  Class& owning_class_;
  Library& owning_lib_;
  Code& code_;
};


void IsolateReloadContext::MarkAllFunctionsForRecompilation() {
  MarkFunctionsForRecompilation visitor(isolate_);
  NoSafepointScope no_safepoint;
  isolate_->heap()->VisitObjects(&visitor);
}


void IsolateReloadContext::InvalidateWorld() {
  // Discard all types of cached lookup, which are all potentially invalid.
  // - ICs and MegamorphicCaches
  // - Optimized code (inlining)
  // - Unoptimized code (unqualifed invocations were early bound to static
  //   or instance invocations)

  DeoptimizeFunctionsOnStack();
  ResetUnoptimizedICsOnStack();
  ResetMegamorphicCaches();
  MarkAllFunctionsForRecompilation();
}


RawClass* IsolateReloadContext::MappedClass(const Class& replacement_or_new) {
  if (class_map_storage_ == Array::null()) {
    return Class::null();
  }
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


RawClass* IsolateReloadContext::LinearFindOldClass(
    const Class& replacement_or_new) {
  const intptr_t lower_cid_bound = Dart::vm_isolate()->class_table()->NumCids();
  const intptr_t upper_cid_bound = saved_num_cids_;
  ClassTable* class_table = I->class_table();
  Class& cls = Class::Handle();
  for (intptr_t i = lower_cid_bound; i < upper_cid_bound; i++) {
    if (!class_table->HasValidClassAt(i)) {
      continue;
    }
    cls = class_table->At(i);
    if (IsSameClass(replacement_or_new, cls)) {
      return cls.raw();
    }
  }
  return Class::null();
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
    old ^= LinearFindOldClass(replacement_or_new);
    if (old.IsNull()) {
      // New class.
      AddClassMapping(replacement_or_new, replacement_or_new);
    } else {
      // Replaced class.
      AddClassMapping(replacement_or_new, old);

      ASSERT(reverse_map_storage_ != Array::null());
      UnorderedHashMap<ReverseMapTraits> reverse_map(reverse_map_storage_);
      ASSERT(reverse_map.FindKey(old) == -1);
      reverse_map.UpdateOrInsert(old, replacement_or_new);
      old.raw()->SetIsReplacedObject();
      reverse_map_storage_ = reverse_map.Release().raw();
    }
  }
}


RawLibrary* IsolateReloadContext::LinearFindOldLibrary(
    const Library& replacement_or_new) {
  const GrowableObjectArray& saved_libs =
      GrowableObjectArray::Handle(saved_libraries());

  Library& lib = Library::Handle();
  for (intptr_t i = 0; i < saved_libs.Length(); i++) {
    lib = Library::RawCast(saved_libs.At(i));
    if (IsSameLibrary(replacement_or_new, lib)) {
      return lib.raw();
    }
  }

  return Library::null();
}


void IsolateReloadContext::BuildLibraryMapping() {
  const GrowableObjectArray& libs =
      GrowableObjectArray::Handle(object_store()->libraries());

  Library& replacement_or_new = Library::Handle();
  Library& old = Library::Handle();
  for (intptr_t i = 0; i < libs.Length(); i++) {
    replacement_or_new = Library::RawCast(libs.At(i));
    if (replacement_or_new.is_dart_scheme()) {
      continue;
    }
    old ^= LinearFindOldLibrary(replacement_or_new);
    if (old.IsNull()) {
      // New library.
      AddLibraryMapping(replacement_or_new, replacement_or_new);
    } else {
      ASSERT(!replacement_or_new.is_dart_scheme());
      // Replaced class.
      AddLibraryMapping(replacement_or_new, old);

      ASSERT(reverse_map_storage_ != Array::null());
      UnorderedHashMap<ReverseMapTraits> reverse_map(reverse_map_storage_);
      ASSERT(reverse_map.FindKey(old) == -1);
      old.raw()->SetIsReplacedObject();
      reverse_map.UpdateOrInsert(old, replacement_or_new);
      reverse_map_storage_ = reverse_map.Release().raw();
    }
  }
}


void IsolateReloadContext::AddClassMapping(const Class& replacement_or_new,
                                           const Class& original) {
  if (class_map_storage_ == Array::null()) {
    // Allocate some initial backing storage.
    class_map_storage_ = HashTables::New<UnorderedHashMap<ClassMapTraits> >(4);
  }
  UnorderedHashMap<ClassMapTraits> map(class_map_storage_);
  // Not present.
  ASSERT(map.FindKey(replacement_or_new) == -1);
  map.UpdateOrInsert(replacement_or_new, original);
  // The storage given to the map may have been reallocated, remember the new
  // address.
  class_map_storage_ = map.Release().raw();
}


void IsolateReloadContext::AddLibraryMapping(const Library& replacement_or_new,
                                             const Library& original) {
  if (library_map_storage_ == Array::null()) {
    // Allocate some initial backing storage.
    library_map_storage_ = HashTables::New<UnorderedHashMap<ClassMapTraits> >(4);
  }
  UnorderedHashMap<LibraryMapTraits> map(library_map_storage_);
  // Not present.
  ASSERT(map.FindKey(replacement_or_new) == -1);
  map.UpdateOrInsert(replacement_or_new, original);
  // The storage given to the map may have been reallocated, remember the new
  // address.
  library_map_storage_ = map.Release().raw();
}


}  // namespace dart