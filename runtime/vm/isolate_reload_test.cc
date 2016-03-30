// Copyright (c) 2016, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "include/dart_api.h"
#include "platform/assert.h"
#include "vm/globals.h"
#include "vm/isolate.h"
#include "vm/lockers.h"
#include "vm/thread_barrier.h"
#include "vm/thread_pool.h"
#include "vm/unit_test.h"

namespace dart {

int64_t SimpleInvoke(Dart_Handle lib, const char* method) {
  Dart_Handle result = Dart_Invoke(lib, NewString(method), 0, NULL);
  EXPECT_VALID(result);
  EXPECT(Dart_IsInteger(result));
  int64_t integer_result = 0;
  result = Dart_IntegerToInt64(result, &integer_result);
  EXPECT_VALID(result);
  return integer_result;
}


TEST_CASE(IsolateReload_FunctionReplacement) {
  const char* kScript =
      "main() {\n"
      "  return 4;\n"
      "}\n"
      "trampoline() => main();\n";

  Dart_Handle lib = TestCase::LoadTestScript(kScript, NULL);
  EXPECT_VALID(lib);

  EXPECT_EQ(4, SimpleInvoke(lib, "trampoline"));

  const char* kReloadScript =
    "main() {\n"
    "  return 10;\n"
    "}\n"
    "trampoline() => main();\n";

  lib = TestCase::ReloadTestScript(kReloadScript);
  EXPECT_VALID(lib);

  EXPECT_EQ(10, SimpleInvoke(lib, "trampoline"));
}


TEST_CASE(IsolateReload_BadClass) {
  const char* kScript =
      "class Foo {\n"
      "  final a;\n"
      "  Foo(this.a);\n"
      "}\n"
      "main() {\n"
      "  new Foo(5);\n"
      "  return 4;\n"
      "}\n"
      "trampoline() => main();\n";

  Dart_Handle lib = TestCase::LoadTestScript(kScript, NULL);
  EXPECT_VALID(lib);

  EXPECT_EQ(4, SimpleInvoke(lib, "trampoline"));

  const char* kReloadScript =
    "class Foo {\n"
    "  final a kjsdf ksjdf ;\n"
    "  Foo(this.a);\n"
    "}\n"
    "main() {\n"
    "  new Foo(5);\n"
    "  return 10;\n"
    "}\n"
    "trampoline() => main();\n";

  Dart_Handle result = TestCase::ReloadTestScript(kReloadScript);
  EXPECT_ERROR(result, "unexpected token");

  EXPECT_EQ(4, SimpleInvoke(lib, "trampoline"));
}

}  // namespace dart
