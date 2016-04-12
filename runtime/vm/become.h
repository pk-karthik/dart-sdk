// Copyright (c) 2016, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef VM_BECOME_H_
#define VM_BECOME_H_

#include "vm/allocation.h"

namespace dart {

class Array;

class Become : public AllStatic {
 public:
  // Smalltalk's one-way bulk become (Array>>#elementsForwardIdentityTo:).
  // Redirects all pointers to elements of 'before' to the corresponding element
  // in 'after'. Every element in 'before' is guarenteed to be dead after this
  // operation, though we won't finalize them until the next GC discovers this.
  // Useful for atomically applying behavior and schema changes.
  static void ElementsForwardIdentity(const Array& before, const Array& after);

  // For completeness, Smalltalk also has a two-way bulk become
  // (Array>>#elementsExchangeIdentityWith:). This is typically used in
  // application-level virtual memory or persistence schemes, where a set of
  // objects are swapped with so-called husks and the original objects are
  // serialized.
  // static void ElementsExchangeIdentity(const Array& before,
  //                                      const Array& after);
};

}  // namespace dart

#endif  // VM_BECOME_H_
