// Copyright (c) 2016, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/object.h"

namespace dart {

void Class::Reload(const Class& replacement) {
}


bool Class::CanReload(const Class& replacement) {
  return true;
}


void Library::Reload(const Library& replacement) {

}


bool Library::CanReload(const Library& replacement) {
  return true;
}

}   // namespace dart.