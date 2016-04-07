// Copyright (c) 2016, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "include/dart_api.h"
#include "include/dart_tools_api.h"
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


const char* SimpleInvokeStr(Dart_Handle lib, const char* method) {
  Dart_Handle result = Dart_Invoke(lib, NewString(method), 0, NULL);
  const char* result_str = NULL;
  EXPECT(Dart_IsString(result));
  EXPECT_VALID(Dart_StringToCString(result, &result_str));
  return result_str;
}


Dart_Handle SimpleInvokeError(Dart_Handle lib, const char* method) {
  Dart_Handle result = Dart_Invoke(lib, NewString(method), 0, NULL);
  EXPECT(Dart_IsError(result));
  return result;
}


TEST_CASE(IsolateReload_FunctionReplacement) {
  const char* kScript =
      "main() {\n"
      "  return 4;\n"
      "}\n";

  Dart_Handle lib = TestCase::LoadTestScript(kScript, NULL);
  EXPECT_VALID(lib);

  EXPECT_EQ(4, SimpleInvoke(lib, "main"));

  const char* kReloadScript =
      "var _unused;"
      "main() {\n"
      "  return 10;\n"
      "}\n";

  lib = TestCase::ReloadTestScript(kReloadScript);
  EXPECT_VALID(lib);

  EXPECT_EQ(10, SimpleInvoke(lib, "main"));
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
      "}\n";

  Dart_Handle lib = TestCase::LoadTestScript(kScript, NULL);
  EXPECT_VALID(lib);

  EXPECT_EQ(4, SimpleInvoke(lib, "main"));

  const char* kReloadScript =
      "var _unused;"
      "class Foo {\n"
      "  final a kjsdf ksjdf ;\n"
      "  Foo(this.a);\n"
      "}\n"
      "main() {\n"
      "  new Foo(5);\n"
      "  return 10;\n"
      "}\n";

  Dart_Handle result = TestCase::ReloadTestScript(kReloadScript);
  EXPECT_ERROR(result, "unexpected token");

  EXPECT_EQ(4, SimpleInvoke(lib, "main"));
}


TEST_CASE(IsolateReload_StaticValuePreserved) {
  const char* kScript =
      "init() => 'old value';\n"
      "var value = init();\n"
      "main() {\n"
      "  return 'init()=${init()},value=${value}';\n"
      "}\n";

  Dart_Handle lib = TestCase::LoadTestScript(kScript, NULL);
  EXPECT_VALID(lib);

  EXPECT_STREQ("init()=old value,value=old value",
               SimpleInvokeStr(lib, "main"));

  const char* kReloadScript =
      "var _unused;"
      "init() => 'new value';\n"
      "var value = init();\n"
      "main() {\n"
      "  return 'init()=${init()},value=${value}';\n"
      "}\n";

  lib = TestCase::ReloadTestScript(kReloadScript);
  EXPECT_VALID(lib);

  EXPECT_STREQ("init()=new value,value=old value",
               SimpleInvokeStr(lib, "main"));
}


TEST_CASE(IsolateReload_SavedClosure) {
  // Create a closure in main which only exists in the original source.
  const char* kScript =
      "magic() {\n"
      "  var x = 'ante';\n"
      "  return x + 'diluvian';\n"
      "}\n"
      "var closure;\n"
      "main() {\n"
      "  closure = () { return magic().toString() + '!'; };\n"
      "  return closure();\n"
      "}\n";

  Dart_Handle lib = TestCase::LoadTestScript(kScript, NULL);
  EXPECT_VALID(lib);

  EXPECT_STREQ("antediluvian!", SimpleInvokeStr(lib, "main"));

  // Remove the original closure from the source code.  The closure is
  // able to be recompiled because its source is preserved in a
  // special patch class.
  const char* kReloadScript =
      "magic() {\n"
      "  return 'postapocalyptic';\n"
      "}\n"
      "var closure;\n"
      "main() {\n"
      "  return closure();\n"
      "}\n";

  lib = TestCase::ReloadTestScript(kReloadScript);
  EXPECT_VALID(lib);

  EXPECT_STREQ("postapocalyptic!", SimpleInvokeStr(lib, "main"));
}


TEST_CASE(IsolateReload_TopLevelFieldAdded) {
  const char* kScript =
      "var value1 = 10;\n"
      "main() {\n"
      "  return 'value1=${value1}';\n"
      "}\n";

  Dart_Handle lib = TestCase::LoadTestScript(kScript, NULL);
  EXPECT_VALID(lib);

  EXPECT_STREQ("value1=10", SimpleInvokeStr(lib, "main"));

  const char* kReloadScript =
      "var value1 = 10;\n"
      "var value2 = 20;\n"
      "main() {\n"
      "  return 'value1=${value1},value2=${value2}';\n"
      "}\n";

  lib = TestCase::ReloadTestScript(kReloadScript);
  EXPECT_VALID(lib);

  EXPECT_STREQ("value1=10,value2=20",
               SimpleInvokeStr(lib, "main"));
}


TEST_CASE(IsolateReload_ClassAdded) {
  const char* kScript =
      "main() {\n"
      "  return 'hello';\n"
      "}\n";

  Dart_Handle lib = TestCase::LoadTestScript(kScript, NULL);
  EXPECT_VALID(lib);

  EXPECT_STREQ("hello", SimpleInvokeStr(lib, "main"));

  const char* kReloadScript =
      "var _unused;"
      "class A {\n"
      "  toString() => 'hello from A';\n"
      "}\n"
      "main() {\n"
      "  return new A().toString();\n"
      "}\n";

  lib = TestCase::ReloadTestScript(kReloadScript);
  EXPECT_VALID(lib);

  EXPECT_STREQ("hello from A", SimpleInvokeStr(lib, "main"));
}


TEST_CASE(IsolateReload_LibraryImportAdded) {
  const char* kScript =
      "main() {\n"
      "  return max(3, 4);\n"
      "}\n";

  Dart_Handle lib = TestCase::LoadTestScript(kScript, NULL);
  EXPECT_VALID(lib);

  EXPECT_ERROR(SimpleInvokeError(lib, "main"), "max");;

  const char* kReloadScript =
      "import 'dart:math';\n"
      "main() {\n"
      "  return max(3, 4);\n"
      "}\n";

  lib = TestCase::ReloadTestScript(kReloadScript);
  EXPECT_VALID(lib);

  EXPECT_EQ(4, SimpleInvoke(lib, "main"));
}


TEST_CASE(IsolateReload_LibraryImportRemoved) {
  const char* kScript =
      "import 'dart:math';\n"
      "main() {\n"
      "  return max(3, 4);\n"
      "}\n";

  Dart_Handle lib = TestCase::LoadTestScript(kScript, NULL);
  EXPECT_VALID(lib);

  EXPECT_EQ(4, SimpleInvoke(lib, "main"));

  const char* kReloadScript =
      "main() {\n"
      "  return max(3, 4);\n"
      "}\n";

  lib = TestCase::ReloadTestScript(kReloadScript);
  EXPECT_VALID(lib);

  EXPECT_ERROR(SimpleInvokeError(lib, "main"), "max");;
}


TEST_CASE(IsolateReload_LibraryDebuggable) {
  const char* kScript =
      "main() {\n"
      "  return 1;\n"
      "}\n";

  Dart_Handle lib = TestCase::LoadTestScript(kScript, NULL);
  EXPECT_VALID(lib);

  // The library is by default debuggable.  Make it not debuggable.
  intptr_t lib_id = -1;
  bool debuggable = false;
  EXPECT_VALID(Dart_LibraryId(lib, &lib_id));
  EXPECT_VALID(Dart_GetLibraryDebuggable(lib_id, &debuggable));
  EXPECT_EQ(true, debuggable);
  EXPECT_VALID(Dart_SetLibraryDebuggable(lib_id, false));
  EXPECT_VALID(Dart_GetLibraryDebuggable(lib_id, &debuggable));
  EXPECT_EQ(false, debuggable);

  EXPECT_EQ(1, SimpleInvoke(lib, "main"));

  const char* kReloadScript =
      "main() {\n"
      "  return 2;\n"
      "}\n";

  lib = TestCase::ReloadTestScript(kReloadScript);
  EXPECT_VALID(lib);

  EXPECT_EQ(2, SimpleInvoke(lib, "main"));

  // Library debuggability is preserved.
  intptr_t new_lib_id = -1;
  EXPECT_VALID(Dart_LibraryId(lib, &new_lib_id));
  EXPECT_VALID(Dart_GetLibraryDebuggable(new_lib_id, &debuggable));
  EXPECT_EQ(false, debuggable);
}


TEST_CASE(IsolateReload_ImplicitConstructorChanged) {
  // Note that we are checking that the value 20 gets cleared from the
  // compile-time constants cache.  To make this test work, "20" and
  // "10" need to be at the same token position.
  const char* kScript =
      "class A {\n"
      "  int field = 20;\n"
      "}\n"
      "var savedA = new A();\n"
      "main() {\n"
      "  var newA = new A();\n"
      "  return 'saved:${savedA.field} new:${newA.field}';\n"
      "}\n";

  Dart_Handle lib = TestCase::LoadTestScript(kScript, NULL);
  EXPECT_VALID(lib);

  EXPECT_STREQ("saved:20 new:20", SimpleInvokeStr(lib, "main"));

  const char* kReloadScript =
      "class A {\n"
      "  int field = 10;\n"
      "}\n"
      "var savedA = new A();\n"
      "main() {\n"
      "  var newA = new A();\n"
      "  return 'saved:${savedA.field} new:${newA.field}';\n"
      "}\n";

  lib = TestCase::ReloadTestScript(kReloadScript);
  EXPECT_VALID(lib);

  EXPECT_STREQ("saved:20 new:10", SimpleInvokeStr(lib, "main"));
}


TEST_CASE(IsolateReload_ConstructorChanged) {
  const char* kScript =
      "class A {\n"
      "  int field;\n"
      "  A() { field = 20; }\n"
      "}\n"
      "var savedA = new A();\n"
      "main() {\n"
      "  var newA = new A();\n"
      "  return 'saved:${savedA.field} new:${newA.field}';\n"
      "}\n";

  Dart_Handle lib = TestCase::LoadTestScript(kScript, NULL);
  EXPECT_VALID(lib);

  EXPECT_STREQ("saved:20 new:20", SimpleInvokeStr(lib, "main"));

  const char* kReloadScript =
      "var _unused;"
      "class A {\n"
      "  int field;\n"
      "  A() { field = 10; }\n"
      "}\n"
      "var savedA = new A();\n"
      "main() {\n"
      "  var newA = new A();\n"
      "  return 'saved:${savedA.field} new:${newA.field}';\n"
      "}\n";

  lib = TestCase::ReloadTestScript(kReloadScript);
  EXPECT_VALID(lib);

  EXPECT_STREQ("saved:20 new:10", SimpleInvokeStr(lib, "main"));
}


TEST_CASE(IsolateReload_SuperClassChanged) {
  const char* kScript =
      "class A {\n"
      "}\n"
      "class B extends A {\n"
      "}\n"
      "var list = [ new A(), new B() ];\n"
      "main() {\n"
      "  return (list.map((x) => '${x is A}/${x is B}')).toString();\n"
      "}\n";

  Dart_Handle lib = TestCase::LoadTestScript(kScript, NULL);
  EXPECT_VALID(lib);

  EXPECT_STREQ("(true/false, true/true)", SimpleInvokeStr(lib, "main"));

  const char* kReloadScript =
      "var _unused;"
      "class B{\n"
      "}\n"
      "class A extends B {\n"
      "}\n"
      "var list = [ new A(), new B() ];\n"
      "main() {\n"
      "  return (list.map((x) => '${x is A}/${x is B}')).toString();\n"
      "}\n";

  lib = TestCase::ReloadTestScript(kReloadScript);
  EXPECT_VALID(lib);

  EXPECT_STREQ("(true/true, false/true)", SimpleInvokeStr(lib, "main"));
}


TEST_CASE(IsolateReload_Generics) {
  // Reload a program with generics without changing the source.  We
  // do this to produce duplication TypeArguments and make sure that
  // the system doesn't die.
  const char* kScript =
      "class A {\n"
      "}\n"
      "class B<T extends A> {\n"
      "}\n"
      "main() {\n"
      "  return new B<A>().toString();\n"
      "}\n";

  Dart_Handle lib = TestCase::LoadTestScript(kScript, NULL);
  EXPECT_VALID(lib);

  EXPECT_STREQ("Instance of 'B<A>'", SimpleInvokeStr(lib, "main"));

  const char* kReloadScript =
      "class A {\n"
      "}\n"
      "class B<T extends A> {\n"
      "}\n"
      "main() {\n"
      "  return new B<A>().toString();\n"
      "}\n";

  lib = TestCase::ReloadTestScript(kReloadScript);
  EXPECT_VALID(lib);

  EXPECT_STREQ("Instance of 'B<A>'", SimpleInvokeStr(lib, "main"));
}


TEST_CASE(IsolateReload_MixinChanged) {
  const char* kScript =
      "class Mixin1 {\n"
      "  var field = 'mixin1';\n"
      "  func() => 'mixin1';\n"
      "}\n"
      "class B extends Object with Mixin1 {\n"
      "}\n"
      "var saved = new B();\n"
      "main() {\n"
      "  return 'saved:field=${saved.field},func=${saved.func()}';\n"
      "}\n";

  Dart_Handle lib = TestCase::LoadTestScript(kScript, NULL);
  EXPECT_VALID(lib);

  EXPECT_STREQ("saved:field=mixin1,func=mixin1",
               SimpleInvokeStr(lib, "main"));

  const char* kReloadScript =
      "class Mixin2 {\n"
      "  var field = 'mixin2';\n"
      "  func() => 'mixin2';\n"
      "}\n"
      "class B extends Object with Mixin2 {\n"
      "}\n"
      "var saved = new B();\n"
      "main() {\n"
      "  var newer = new B();\n"
      "  return 'saved:field=${saved.field},func=${saved.func()} '\n"
      "         'newer:field=${newer.field},func=${newer.func()}';\n"
      "}\n";

  lib = TestCase::ReloadTestScript(kReloadScript);
  EXPECT_VALID(lib);

  // The saved instance of B retains its old field value from mixin1,
  // but it gets the new implementation of func from mixin2.
  EXPECT_STREQ("saved:field=mixin1,func=mixin2 "
               "newer:field=mixin2,func=mixin2",
               SimpleInvokeStr(lib, "main"));
}


TEST_CASE(IsolateReload_ComplexInheritanceChange) {
  const char* kScript =
      "class A {\n"
      "  String name;\n"
      "  A(this.name);\n"
      "}\n"
      "class B extends A {\n"
      "  B(name) : super(name);\n"
      "}\n"
      "class C extends B {\n"
      "  C(name) : super(name);\n"
      "}\n"
      "var list = [ new A('a'), new B('b'), new C('c') ];\n"
      "main() {\n"
      "  return (list.map((x) {\n"
      "    return '${x.name} is A(${x is A})/ B(${x is B})/ C(${x is C})';\n"
      "  })).toString();\n"
      "}\n";

  Dart_Handle lib = TestCase::LoadTestScript(kScript, NULL);
  EXPECT_VALID(lib);

  EXPECT_STREQ("(a is A(true)/ B(false)/ C(false),"
               " b is A(true)/ B(true)/ C(false),"
               " c is A(true)/ B(true)/ C(true))",
               SimpleInvokeStr(lib, "main"));

  const char* kReloadScript =
      "class C {\n"
      "  String name;\n"
      "  C(this.name);\n"
      "}\n"
      "class X extends C {\n"
      "  X(name) : super(name);\n"
      "}\n"
      "class A extends X {\n"
      "  A(name) : super(name);\n"
      "}\n"
      "var list;\n"
      "main() {\n"
      "  list.add(new X('x'));\n"
      "  return (list.map((x) {\n"
      "    return '${x.name} is A(${x is A})/ C(${x is C})/ X(${x is X})';\n"
      "  })).toString();\n"
      "}\n";

  lib = TestCase::ReloadTestScript(kReloadScript);
  EXPECT_VALID(lib);

  EXPECT_STREQ("(a is A(true)/ C(true)/ X(true),"
               " b is A(true)/ C(true)/ X(true),"  // still extends A...
               " c is A(false)/ C(true)/ X(false),"
               " x is A(false)/ C(true)/ X(true))",
               SimpleInvokeStr(lib, "main"));

  // Revive the class B and make sure all allocated instances take
  // their place in the inheritance hierarchy.
  const char* kReloadScript2 =
      "class X {\n"
      "  String name;\n"
      "  X(this.name);\n"
      "}\n"
      "class A extends X{\n"
      "  A(name) : super(name);\n"
      "}\n"
      "class B extends X {\n"
      "  B(name) : super(name);\n"
      "}\n"
      "class C extends A {\n"
      "  C(name) : super(name);\n"
      "}\n"
      "var list;\n"
      "main() {\n"
      "  return (list.map((x) {\n"
      "    return '${x.name} is '\n"
      "           'A(${x is A})/ B(${x is B})/ C(${x is C})/ X(${x is X})';\n"
      "  })).toString();\n"
      "}\n";

  lib = TestCase::ReloadTestScript(kReloadScript2);
  EXPECT_VALID(lib);

  EXPECT_STREQ("(a is A(true)/ B(false)/ C(false)/ X(true),"
               " b is A(false)/ B(true)/ C(false)/ X(true),"
               " c is A(true)/ B(false)/ C(true)/ X(true),"
               " x is A(false)/ B(false)/ C(false)/ X(true))",
               SimpleInvokeStr(lib, "main"));
}


TEST_CASE(IsolateReload_LiveStack) {
  const char* kScript =
      "import 'isolate_reload_test_helper';\n"
      "helper() => 7;\n"
      "alpha() { var x = helper(); reloadTest(); return x + helper(); }\n"
      "foo() => alpha();\n"
      "bar() => foo();\n"
      "main() {\n"
      "  return bar();\n"
      "}\n";

  Dart_Handle lib = TestCase::LoadTestScript(kScript, NULL);
  EXPECT_VALID(lib);

  const char* kReloadScript =
      "import 'isolate_reload_test_helper';\n"
      "helper() => 100;\n"
      "alpha() => 5 + helper();\n"
      "foo() => alpha();\n"
      "bar() => foo();\n"
      "main() {\n"
      "  return bar();\n"
      "}\n";

  TestCase::SetReloadTestScript(kReloadScript);

  EXPECT_EQ(107, SimpleInvoke(lib, "main"));

  lib = TestCase::GetReloadErrorOrRootLibrary();
  EXPECT_VALID(lib);

  EXPECT_EQ(105, SimpleInvoke(lib, "main"));
}


TEST_CASE(IsolateReload_LibraryLookup) {
  const char* kScript =
      "main() {\n"
      "  return importedFunc();\n"
      "}\n";

  Dart_Handle result;

  Dart_Handle lib = TestCase::LoadTestScript(kScript, NULL);
  EXPECT_VALID(lib);

  EXPECT_ERROR(SimpleInvokeError(lib, "main"), "importedFunc");

  // Fail to find 'importable_test_lib' in the isolate.
  result = Dart_LookupLibrary(NewString("importable_test_lib"));
  EXPECT(Dart_IsError(result));

  const char* kReloadScript =
      "import 'importable_test_lib';\n"
      "main() {\n"
      "  return importedFunc();\n"
      "}\n";

  // Reload and add 'importable_test_lib' to isolate.
  lib = TestCase::ReloadTestScript(kReloadScript);
  EXPECT_VALID(lib);

  EXPECT_STREQ("a", SimpleInvokeStr(lib, "main"));

  // Find 'importable_test_lib' in the isolate.
  result = Dart_LookupLibrary(NewString("importable_test_lib"));
  EXPECT(Dart_IsLibrary(result));

  // Reload and remove 'dart:math' from isolate.
  lib = TestCase::ReloadTestScript(kScript);
  EXPECT_VALID(lib);

  // Fail to find 'importable_test_lib' in the isolate.
  result = Dart_LookupLibrary(NewString("importable_test_lib"));
  EXPECT(Dart_IsError(result));
}


TEST_CASE(IsolateReload_LibraryHide) {
  // Import 'importable_test_lib' with importedFunc hidden. Will result in an
  // error.
  const char* kScript =
      "import 'importable_test_lib' hide importedFunc;\n"
      "main() {\n"
      "  return importedFunc();\n"
      "}\n";

  // Dart_Handle result;

  Dart_Handle lib = TestCase::LoadTestScript(kScript, NULL);
  EXPECT_VALID(lib);

  EXPECT_ERROR(SimpleInvokeError(lib, "main"), "importedFunc");

  // Import 'importable_test_lib'.
  const char* kReloadScript =
      "import 'importable_test_lib';\n"
      "main() {\n"
      "  return importedFunc();\n"
      "}\n";

  lib = TestCase::ReloadTestScript(kReloadScript);
  EXPECT_VALID(lib);

  EXPECT_STREQ("a", SimpleInvokeStr(lib, "main"));
}


TEST_CASE(IsolateReload_LibraryShow) {
  // Import 'importable_test_lib' with importedIntFunc visible. Will result in
  // an error when 'main' is invoked.
  const char* kScript =
      "import 'importable_test_lib' show importedIntFunc;\n"
      "main() {\n"
      "  return importedFunc();\n"
      "}\n"
      "mainInt() {\n"
      "  return importedIntFunc();\n"
      "}\n";


  Dart_Handle lib = TestCase::LoadTestScript(kScript, NULL);
  EXPECT_VALID(lib);

  // Works.
  EXPECT_EQ(4, SimpleInvoke(lib, "mainInt"));
  // Results in an error.
  EXPECT_ERROR(SimpleInvokeError(lib, "main"), "importedFunc");

  // Import 'importable_test_lib' with importedFunc visible. Will result in
  // an error when 'mainInt' is invoked.
  const char* kReloadScript =
      "import 'importable_test_lib' show importedFunc;\n"
      "main() {\n"
      "  return importedFunc();\n"
      "}\n"
      "mainInt() {\n"
      "  return importedIntFunc();\n"
      "}\n";

  lib = TestCase::ReloadTestScript(kReloadScript);
  EXPECT_VALID(lib);

  // Works.
  EXPECT_STREQ("a", SimpleInvokeStr(lib, "main"));
  // Results in an error.
  EXPECT_ERROR(SimpleInvokeError(lib, "mainInt"), "importedIntFunc");
}


// Verifies that we clear the ICs for the functions live on the stack in a way
// that is compatible with the fast path smi stubs.
TEST_CASE(IsolateReload_SmiFastPathStubs) {
  const char* kScript =
      "import 'isolate_reload_test_helper';\n"
      "import 'importable_test_lib' show importedIntFunc;\n"
      "main() {\n"
      "  var x = importedIntFunc();\n"
      "  var y = importedIntFunc();\n"
      "  reloadTest();\n"
      "  return x + y;\n"
      "}\n";


  Dart_Handle lib = TestCase::LoadTestScript(kScript, NULL);
  EXPECT_VALID(lib);

  // Identity reload.
  TestCase::SetReloadTestScript(kScript);

  EXPECT_EQ(8, SimpleInvoke(lib, "main"));
}


TEST_CASE(IsolateReload_TopLevelParseError) {
  const char* kScript =
      "main() {\n"
      "  return 4;\n"
      "}\n";

  Dart_Handle lib = TestCase::LoadTestScript(kScript, NULL);
  EXPECT_VALID(lib);

  EXPECT_EQ(4, SimpleInvoke(lib, "main"));

  const char* kReloadScript =
      "kjsadkfjaksldfjklsadf;\n"
      "main() {\n"
      "  return 4;\n"
      "}\n";

  lib = TestCase::ReloadTestScript(kReloadScript);
  EXPECT_ERROR(lib, "unexpected token");
}

}  // namespace dart
