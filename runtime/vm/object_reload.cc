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
DECLARE_FLAG(bool, two_args_smi_icd);

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


void Function::ZeroEdgeCounters() const {
  const Array& saved_ic_data = Array::Handle(ic_data_array());
  if (saved_ic_data.IsNull()) {
    return;
  }
  const intptr_t saved_ic_datalength = saved_ic_data.Length();
  ASSERT(saved_ic_datalength > 0);
  const Array& edge_counters_array =
      Array::Handle(Array::RawCast(saved_ic_data.At(0)));
  ASSERT(!edge_counters_array.IsNull());
  // Fill edge counters array with zeros.
  const Smi& zero = Smi::Handle(Smi::New(0));
  for (intptr_t i = 0; i < edge_counters_array.Length(); i++) {
    edge_counters_array.SetAt(i, zero);
  }
}


static void ClearICs(const Function& function, const Code& code) {
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


void Function::FillICDataWithSentinels(const Code& code) const {
  ASSERT(code.raw() == CurrentCode());
  ClearICs(*this, code);
}


void Class::CopyStaticFieldValues(const Class& old_cls) const {
  IsolateReloadContext* reload_context = Isolate::Current()->reload_context();
  ASSERT(reload_context != NULL);

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
          reload_context->AddStaticFieldMapping(old_field, field);
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


bool Class::CanReload(const Class& replacement) const {
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

  if (is_finalized()) {
    // Get the field maps for both classes. These field maps walk the class
    // hierarchy.
    const Array& fields =
        Array::Handle(OffsetToFieldMap());
    const Array& replacement_fields =
        Array::Handle(replacement.OffsetToFieldMap());

    // Check that we have the same number of fields.
    if (fields.Length() != replacement_fields.Length()) {
      IRC->ReportError(String::Handle(String::NewFormatted(
          "Number of instance fields changed in %s", ToCString())));
      return false;
    }

    // Verify that field names / offsets match across the entire hierarchy.
    Field& field = Field::Handle();
    String& field_name = String::Handle();
    Field& replacement_field = Field::Handle();
    String& replacement_field_name = String::Handle();
    for (intptr_t i = 0; i < fields.Length(); i++) {
      if (fields.At(i) == Field::null()) {
        ASSERT(replacement_fields.At(i) == Field::null());
        continue;
      }
      field = Field::RawCast(fields.At(i));
      replacement_field = Field::RawCast(replacement_fields.At(i));
      field_name = field.name();
      replacement_field_name = replacement_field.name();
      if (!field_name.Equals(replacement_field_name)) {
        IRC->ReportError(String::Handle(String::NewFormatted(
            "Name of instance field changed ('%s' vs '%s') in '%s'",
            field_name.ToCString(),
            replacement_field_name.ToCString(),
            ToCString())));
        return false;
      }
    }
  } else if (is_prefinalized()) {
    if (!replacement.is_prefinalized()) {
      IRC->ReportError(String::Handle(String::NewFormatted(
          "Original class ('%s') is prefinalized and replacement class ('%s')",
          ToCString(), replacement.ToCString())));
      return false;
    }
    if (instance_size() != replacement.instance_size()) {
     IRC->ReportError(String::Handle(String::NewFormatted(
         "Instance size mismatch between '%s' (%" Pd ") and replacement "
         "'%s' ( %" Pd ")",
         ToCString(),
         instance_size(),
         replacement.ToCString(),
         replacement.instance_size())));
     return false;
    }
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


bool Library::CanReload(const Library& replacement) const {
  return true;
}


void ICData::Reset(bool is_static_call) const {
  if (is_static_call) {
    const Function& old_target = Function::Handle(GetTargetAt(0));
    ASSERT(!old_target.IsNull());
    if (!old_target.is_static()) {
      // TODO(johnmccutchan): Improve this.
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
      // TODO(johnmccutchan): Improve this.
      TIR_Print("Cannot rebind static call to %s from %s\n",
                old_target.ToCString(),
                Object::Handle(Owner()).ToCString());
      return;
    }
    ClearAndSetStaticTarget(new_target);
  } else {
    ClearWithSentinel();
  }
}


}   // namespace dart.
