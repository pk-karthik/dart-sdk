// Copyright (c) 2016, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/object.h"

#include "vm/isolate_reload.h"
#include "vm/log.h"
#include "vm/resolver.h"
#include "vm/symbols.h"

namespace dart {

DECLARE_FLAG(bool, trace_reload);

#define IRC (Isolate::Current()->reload_context())

class ObjectReloadUtils : public AllStatic {
  static void DumpLibraryDictionary(const Library& lib) {
    DictionaryIterator it(lib);
    Object& entry = Object::Handle();
    String& name = String::Handle();
    TIR_Print("Dumping dictionary for %s\n", lib.ToCString());
    while (it.HasNext()) {
      entry = it.GetNext();
      name = entry.DictionaryName();
      TIR_Print("%s -> %s\n", name.ToCString(), entry.ToCString());
    }
  }
};


void Function::Reparent(const Class& new_cls) const {
  set_owner(new_cls);
}


void Class::CopyStaticFieldValues(const Class& old_cls) const {
  const Array& old_field_list = Array::Handle(old_cls.fields());
  Field& old_field = Field::Handle();
  String& old_name = String::Handle();

  const Array& field_list = Array::Handle(fields());
  Field& field = Field::Handle();
  String& name = String::Handle();

  Instance& value = Instance::Handle();
  for (intptr_t i = 0; i < field_list.Length(); i++) {
    field = Field::RawCast(field_list.At(i));
    name = field.name();
    if (field.is_static()) {
      // Find the corresponding old field, if it exists, and migrate
      // over the field value.
      for (intptr_t j = 0; j < old_field_list.Length(); j++) {
        old_field = Field::RawCast(old_field_list.At(j));
        old_name = old_field.name();
        if (name.Equals(old_name)) {
          value = old_field.StaticValue();
          field.SetStaticValue(value);
        }
      }
    }
  }

}


void Class::PatchFieldsAndFunctions() const {
  // Move all old functions and fields to a patch class so that they
  // still refer to their original script.
  const PatchClass& patch =
      PatchClass::Handle(PatchClass::New(*this, Script::Handle(script())));

  const Array& funcs = Array::Handle(functions());
  Function& func = Function::Handle();
  for (intptr_t i = 0; i < funcs.Length(); i++) {
    func = Function::RawCast(funcs.At(i));
    func.set_owner(patch);
  }

  const Array& old_field_list = Array::Handle(fields());
  Field& old_field = Field::Handle();
  for (intptr_t i = 0; i < old_field_list.Length(); i++) {
    old_field = Field::RawCast(old_field_list.At(i));
    old_field.set_owner(patch);
  }
}


void Class::Reload(const Class& replacement) {
  // TODO(turnidge): This method is incomplete.
  //
  // CHECKLIST (by field from RawClass);
  //
  // - name_ : DONE, checked in CanReload
  // - library : DONE, checked in CanReload
  // - functions : DONE
  // - fields : DONE
  // - script : DONE
  // - token_pos : DONE
  // - instance_size_in_words : DONE, implicitly (needs assert)
  // - id : DONE, because we are copying into existing class
  // - canonical_types : currently assuming all are of type Type.  Is this ok?
  // - super_type: DONE
  // - constants : DONE, leave these alone.
  // - allocation_stub : DONE for now.  Revisit later.
  //
  // - mixin : todo
  // - functions_hash_table : todo
  // - offset_in_words_to_field : todo
  // - interfaces : todo
  // - type_parameters : todo
  // - signature_function : todo
  // - invocation_dispatcher_cache : todo
  // - direct_subclasses : todo
  // - cha_codes : todo
  // - handle_vtable : todo
  // - type_arguments_field... : todo
  // - next_field_offset... : todo
  // - num_type_arguments : todo
  // - num_own_type_arguments : todo
  // - num_native_fields : todo
  // - state_bits : todo

  // Move all old functions and fields to a patch class so that they
  // still refer to their original script.
  PatchFieldsAndFunctions();

  // Move new functions to the old class.
  const Array& funcs = Array::Handle(replacement.functions());
  Function& func = Function::Handle();
  for (intptr_t i = 0; i < funcs.Length(); i++) {
    func ^= funcs.At(i);
    func.Reparent(*this);
  }
  SetFunctions(Array::Handle(replacement.functions()));

  // Move new fields to the old class, preserving static field values.
  replacement.CopyStaticFieldValues(*this);
  SetFields(Array::Handle(replacement.fields()));

  // TODO(turnidge): Do we really need to do this here?
  DisableAllocationStub();

  // Replace script
  set_script(Script::Handle(replacement.script()));
  set_token_pos(replacement.token_pos());

  // Update the canonical type(s).
  const Object& types_obj = Object::Handle(replacement.canonical_types());
  Type& type = Type::Handle();
  if (!types_obj.IsNull()) {
    if (types_obj.IsType()) {
      type ^= types_obj.raw();
      type.set_type_class(*this);
    } else {
      const Array& types = Array::Cast(types_obj);
      for (intptr_t i = 0; i < types.Length(); i++) {
        type ^= types.At(i);
        type.set_type_class(*this);
      }
    }
  }

  // Update supertype.
  set_super_type(AbstractType::Handle(replacement.super_type()));
}


bool Class::CanReload(const Class& replacement) {
#if defined(DEBUG)
  {
    ASSERT(IsolateReloadContext::IsSameClass(*this, replacement));
  }
#endif

  if (is_finalized()) {
    const Error& error =
        Error::Handle(replacement.EnsureIsFinalized(Thread::Current()));
    if (!error.IsNull()) {
      IRC->ReportError(error);
      return false;
    }
  }
  // field count check.
  // TODO(johnmccutchan): Check super class sizes and fields as well.
  // TODO(johnmccutchan): Verify that field names and storage offsets match.
  if (NumInstanceFields() != replacement.NumInstanceFields()) {
    IRC->ReportError(String::Handle(String::NewFormatted(
        "Number of instance fields changed in %s", ToCString())));
    return false;
  }

  // native field count check.
  if (num_native_fields() != replacement.num_native_fields()) {
    IRC->ReportError(String::Handle(String::NewFormatted(
        "Number of native fields changed in %s", ToCString())));
    return false;
  }
  // TODO type parameter count check.
  return true;
}


#if 0
Library:

COPY RawGrowableObjectArray* metadata_;  // Metadata on classes, methods etc.
COPY RawGrowableObjectArray* patch_classes_;

?? RawInstance* load_error_;     // Error iff load_state_ == kLoadError.
?? classid_t index_;             // Library id number.
?? int8_t load_state_;           // Of type LibraryState.
?? bool debuggable_;             // True if debugger can stop in library.
?? bool is_in_fullsnapshot_;     // True if library is in a full snapshot.

Namespace:

UPDATE RawLibrary* library_;          // library with name dictionary.
?? RawArray* show_names_;         // list of names that are exported.
?? RawArray* hide_names_;         // blacklist of names that are not exported.
?? RawField* metadata_field_;     // remembers the token pos of metadata if any,
                                  // and the metadata values if computed.
#endif

void Library::Reload(const Library& replacement) {
  // Hollow out the library.
  StorePointer(&raw_ptr()->loaded_scripts_, Array::null());
  InvalidateResolvedNamesCache();
  InitClassDictionary();
  // Clears imports and exports.
  DropDependencies();
  InitImportList();

  set_corelib_imported(replacement.corelib_imported());

  // Add imports.
  Namespace& ns = Namespace::Handle();
  const Array& imports = Array::Handle(replacement.imports());
  for (intptr_t i = 0; i < imports.Length(); i++) {
    if (imports.At(i) == Object::null()) {
      // Skip null imports.
      // TODO(johnmccutchan): Why does the imports list have a null entry?
      continue;
    }
    ns = Namespace::RawCast(imports.At(i));
    AddImport(ns);
  }

  // Add exports.
  if (replacement.HasExports()) {
    const Array& exports = Array::Handle(replacement.exports());
    for (intptr_t i = 0; i < exports.Length(); i++) {
      ns = Namespace::RawCast(exports.At(i));
      AddExport(ns);
    }
  }

  // Migrate the dictionary from the replacement library back to the original.
  DictionaryIterator it(replacement);
  Object& entry = Object::Handle();
  Class& cls = Class::Handle();
  String& name = String::Handle();
  while (it.HasNext()) {
    entry = it.GetNext();
    if (entry.IsClass()) {
      cls = IRC->FindOriginalClass(Class::Cast(entry));
      if (cls.IsNull()) {
        // This class is new to the library.
        AddClass(Class::Cast(entry));
      } else {
        // This class was in the library already.
        AddClass(cls);
      }
    } else {
      name = entry.DictionaryName();
      AddObject(entry, name);
    }
  }


}


bool Library::CanReload(const Library& replacement) {
  return true;
}


void ICData::Reset(bool is_static_call) const {
  if (is_static_call) {
    const Function& old_target = Function::Handle(GetTargetAt(0));
    ASSERT(!old_target.IsNull());
    if (!old_target.is_static()) {
      TIR_Print("Cannot rebind super-call to %s from %s\n",
                old_target.ToCString(),
                Object::Handle(Owner()).ToCString());
      return;
    }
    const String& selector = String::Handle(old_target.name());
    const Class& cls = Class::Handle(old_target.Owner());
    const Function& new_target =
        Function::Handle(cls.LookupStaticFunction(selector));
    if (new_target.IsNull()) {
      TIR_Print("Cannot rebind static call to %s from %s\n",
                old_target.ToCString(),
                Object::Handle(Owner()).ToCString());
      return;
    }
    ResetData();
    AddTarget(new_target);
  } else {
    ResetData();

    // Restore static prediction that + - = have smi receiver and argument.
    // Cf. TwoArgsSmiOpInlineCacheEntry
    if ((NumArgsTested() == 2) /*&& FLAG_two_args_smi_icd*/) {
      const String& selector = String::Handle(target_name());
      if ((selector.raw() == Symbols::Plus().raw()) ||
          (selector.raw() == Symbols::Minus().raw()) ||
          (selector.raw() == Symbols::Equals().raw())) {
        const Class& smi_class = Class::Handle(Smi::Class());
        const Function& smi_op_target = Function::Handle(
            Resolver::ResolveDynamicAnyArgs(smi_class, selector));
        GrowableArray<intptr_t> class_ids(2);
        class_ids.Add(kSmiCid);
        class_ids.Add(kSmiCid);
        AddCheck(class_ids, smi_op_target);
      }
    }
  }
}


}   // namespace dart.
