// Copyright (c) 2016, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/become.h"

#include "platform/assert.h"
#include "platform/utils.h"
#include "vm/raw_object.h"
#include "vm/object.h"
#include "vm/safepoint.h"
#include "vm/timeline.h"
#include "vm/freelist.h"
#include "vm/visitor.h"

namespace dart {

class ForwardPointersVisitor : public ObjectPointerVisitor {
 public:
  explicit ForwardPointersVisitor(Isolate* isolate)
      : ObjectPointerVisitor(isolate) { }

  virtual void VisitPointers(RawObject** first, RawObject** last) {
    for (RawObject** p = first; p <= last; p++) {
      RawObject* old_target = *p;
      if (old_target->IsHeapObject() &&
          old_target->IsFreeListElement()) {
        uword addr = reinterpret_cast<uword>(old_target) - kHeapObjectTag;
        FreeListElement* forwarder = reinterpret_cast<FreeListElement*>(addr);
        RawObject* new_target = reinterpret_cast<RawObject*>(forwarder->next());
        *p = new_target;
      }
    }
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(ForwardPointersVisitor);
};


#if defined(DEBUG)
class NoFreeListTargetsVisitor : public ObjectPointerVisitor {
 public:
  explicit NoFreeListTargetsVisitor(Isolate* isolate)
      : ObjectPointerVisitor(isolate) { }

  virtual void VisitPointers(RawObject** first, RawObject** last) {
    for (RawObject** p = first; p <= last; p++) {
      RawObject* target = *p;
      if (target->IsHeapObject()) {
        ASSERT(!target->IsFreeListElement());
      }
    }
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(NoFreeListTargetsVisitor);
};
#endif


void Become::ElementsForwardIdentity(const Array& before, const Array& after) {
  Thread* thread = Thread::Current();
  Isolate* isolate = thread->isolate();
  Heap* heap = isolate->heap();

  TIMELINE_FUNCTION_GC_DURATION(thread, "Become::ElementsForwardIdentity");
  SafepointOperationScope safepoint_scope(thread);
  NoSafepointScope no_safepoints;

#if defined(DEBUG)
  {
    // There should be no pointers to free list elements / forwarding corpses.
    NoFreeListTargetsVisitor visitor(isolate);
    isolate->VisitObjectPointers(&visitor, true);
    heap->VisitObjectPointers(&visitor);
  }
#endif

  // Setup forwarding pointers.
  ASSERT(before.Length() == after.Length());
  for (intptr_t i = 0; i < before.Length(); i++) {
    RawObject* before_obj = before.At(i);
    RawObject* after_obj = after.At(i);

    if (before_obj == after_obj) {
      FATAL("become: Cannot self-forward");
    }
    if (!before_obj->IsHeapObject()) {
      FATAL("become: Cannot forward immediates");
    }
    if (before_obj->IsVMHeapObject()) {
      FATAL("become: Cannot forward VM heap objects");
    }
    if (before_obj->IsFreeListElement()) {
      FATAL("become: Cannot forward to multiple objects");
    }
    if (after_obj->IsFreeListElement()) {
      // The Smalltalk become does allow this, and for very special cases
      // it is important (shape changes to Class or Mixin), but as these
      // cases do not arise in Dart, better to prohibit it.
      FATAL("become: No indirect chains of forwarding");
    }
    if (before_obj->IsOldObject() && !after_obj->IsSmiOrOldObject()) {
      UNIMPLEMENTED();  // Requires store buffer update.
    }

    intptr_t size_before = before_obj->Size();

    // TODO(rmacnak): We should use different cids for forwarding corpses and
    // free list elements.
    uword corpse_addr = reinterpret_cast<uword>(before_obj) - kHeapObjectTag;
    FreeListElement* forwarder = FreeListElement::AsElement(corpse_addr,
                                                            size_before);
    forwarder->set_next(reinterpret_cast<FreeListElement*>(after_obj));

    // Still need to be able to iterate over the forwarding corpse.
    intptr_t size_after = before_obj->Size();
    ASSERT(size_before == size_after);
  }

  {
    // Follow forwarding pointers.
    ForwardPointersVisitor visitor(isolate);
    isolate->VisitObjectPointers(&visitor, true);
    heap->VisitObjectPointers(&visitor);
  }

#if defined(DEBUG)
  for (intptr_t i = 0; i < before.Length(); i++) {
    ASSERT(before.At(i) == after.At(i));
  }

  {
    // There should be no pointers to forwarding corpses.
    NoFreeListTargetsVisitor visitor(isolate);
    isolate->VisitObjectPointers(&visitor, true);
    heap->VisitObjectPointers(&visitor);
  }
#endif
}

}  // namespace dart
