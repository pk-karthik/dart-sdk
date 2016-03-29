// Copyright (c) 2016, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/object.h"
#include "vm/resolver.h"
#include "vm/symbols.h"

namespace dart {

void Function::Reparent(const Class& new_cls) const {
  set_owner(new_cls);
}


void Class::Reload(const Class& replacement) {
  if (is_finalized()) {
    replacement.EnsureIsFinalized(Thread::Current());
  }

  // Create a new patch class for the original source.
  const PatchClass& patch =
      PatchClass::Handle(PatchClass::New(*this, Script::Handle(script())));
  Function& func = Function::Handle();
  Array& funcs = Array::Handle(functions());
  for (intptr_t i = 0; i < funcs.Length(); i++) {
    func ^= funcs.At(i);
    func.set_owner(patch);
  }

  // replace functions
  funcs = replacement.functions();
  for (intptr_t i = 0; i < funcs.Length(); i++) {
    func ^= funcs.At(i);
    func.Reparent(*this);
  }
  SetFunctions(Array::Handle(replacement.functions()));

  // replace script
  set_script(Script::Handle(replacement.script()));
  set_token_pos(replacement.token_pos());
  // replace library
  // clear some stuff

  // static fields
  // class hierarchy changes
}


bool Class::CanReload(const Class& replacement) {
  // field count check.
  return true;
}


void Library::Reload(const Library& replacement) {
  StorePointer(&raw_ptr()->loaded_scripts_, Array::null());
}


bool Library::CanReload(const Library& replacement) {
  return true;
}


void ICData::Reset(bool is_static_call) const {
  if (is_static_call) {
    const Function& old_target = Function::Handle(GetTargetAt(0));
    ASSERT(!old_target.IsNull());
    if (!old_target.is_static()) {
      OS::Print("Cannot rebind super-call to %s from %s\n",
                old_target.ToCString(),
                Object::Handle(Owner()).ToCString());
      return;
    }
    const String& selector = String::Handle(old_target.name());
    const Class& cls = Class::Handle(old_target.Owner());
    const Function& new_target =
        Function::Handle(cls.LookupStaticFunction(selector));
    if (new_target.IsNull()) {
      OS::Print("Cannot rebind static call to %s from %s\n",
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