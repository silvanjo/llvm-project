// REQUIRES: aarch64-registered-target

// No MTE, so no StackSafety.

// RUN: rm -f %t*

// -O1, no tagging
// RUN: %clang -O1 --target=aarch64-unknown-linux -mllvm -stack-safety-print %s -S -o - 2>&1 | FileCheck %s

// Full LTO
// RUN: %clang -O1 --target=aarch64-unknown-linux -c %s -Xclang -opaque-pointers -flto=full -o %t.ltonewpm1.bc
// RUN: %clang -O1 --target=aarch64-unknown-linux -c -DBUILD2 %s -Xclang -opaque-pointers -flto=full -o %t.ltonewpm2.bc
// RUN: llvm-lto2 run -o %t.ltonewpm %t.ltonewpm1.bc %t.ltonewpm2.bc -save-temps -stack-safety-print -thinlto-threads 1 -O1 \
// RUN:  -r %t.ltonewpm1.bc,fn,plx \
// RUN:  -r %t.ltonewpm1.bc,use,lx \
// RUN:  -r %t.ltonewpm1.bc,use_local,plx \
// RUN:  -r %t.ltonewpm1.bc,w, \
// RUN:  -r %t.ltonewpm2.bc,use,plx \
// RUN:  -r %t.ltonewpm2.bc,z, 2>&1 | FileCheck %s --allow-empty

// Thin LTO, new PM
// RUN: %clang -O1 --target=aarch64-unknown-linux -c %s -Xclang -opaque-pointers -flto=thin -o %t.thinltonewpm1.bc
// RUN: %clang -O1 --target=aarch64-unknown-linux -c -DBUILD2 %s -Xclang -opaque-pointers -flto=thin -o %t.thinltonewpm2.bc
// RUN: llvm-lto2 run -o %t.thinltonewpm %t.thinltonewpm1.bc %t.thinltonewpm2.bc -save-temps -stack-safety-print -thinlto-threads 1 -O1 \
// RUN:  -r %t.thinltonewpm1.bc,fn,plx \
// RUN:  -r %t.thinltonewpm1.bc,use,lx \
// RUN:  -r %t.thinltonewpm1.bc,use_local,plx \
// RUN:  -r %t.thinltonewpm1.bc,w, \
// RUN:  -r %t.thinltonewpm2.bc,use,plx \
// RUN:  -r %t.thinltonewpm2.bc,z, 2>&1 | FileCheck %s --allow-empty

// Now with MTE.
// RUN: rm -f %t*

// -O0: both are unsafe.
// RUN: %clang -O0 --target=aarch64-unknown-linux -march=armv8+memtag -fsanitize=memtag-stack -mllvm -stack-safety-print %s -S -o - 2>&1 | FileCheck %s

// No LTO: just one is safe.
// RUN: %clang -O1 --target=aarch64-unknown-linux -march=armv8+memtag -fsanitize=memtag-stack -mllvm -stack-safety-print %s -S -o /dev/null 2>&1 | FileCheck %s -check-prefixes=SSI,XUNSAFE,YSAFE

// Full LTO: both are safe.
// RUN: %clang -O1 --target=aarch64-unknown-linux -march=armv8+memtag -fsanitize=memtag-stack -c %s -Xclang -opaque-pointers -flto=full -o %t.ltonewpm1.bc
// RUN: %clang -O1 --target=aarch64-unknown-linux -march=armv8+memtag -fsanitize=memtag-stack -c -DBUILD2 %s -Xclang -opaque-pointers -flto=full -o %t.ltonewpm2.bc
// RUN: llvm-lto2 run -o %t.ltonewpm %t.ltonewpm1.bc %t.ltonewpm2.bc -save-temps -stack-safety-print -thinlto-threads 1 -O1 \
// RUN:  -r %t.ltonewpm1.bc,fn,plx \
// RUN:  -r %t.ltonewpm1.bc,use,lx \
// RUN:  -r %t.ltonewpm1.bc,use_local,plx \
// RUN:  -r %t.ltonewpm1.bc,w, \
// RUN:  -r %t.ltonewpm2.bc,use,plx \
// RUN:  -r %t.ltonewpm2.bc,z, 2>&1 | FileCheck %s -check-prefixes=SSI,XSAFE,YSAFE

// Thin LTO: both are safe.
// RUN: %clang -O1 --target=aarch64-unknown-linux -march=armv8+memtag -fsanitize=memtag-stack -c %s -Xclang -opaque-pointers -flto=thin -o %t.thinltonewpm1.bc
// RUN: %clang -O1 --target=aarch64-unknown-linux -march=armv8+memtag -fsanitize=memtag-stack -c -DBUILD2 %s -Xclang -opaque-pointers -flto=thin -o %t.thinltonewpm2.bc
// RUN: llvm-lto2 run -o %t.thinltonewpm %t.thinltonewpm1.bc %t.thinltonewpm2.bc -save-temps -stack-safety-print -thinlto-threads 1 -O1 \
// RUN:  -r %t.thinltonewpm1.bc,fn,plx \
// RUN:  -r %t.thinltonewpm1.bc,use,lx \
// RUN:  -r %t.thinltonewpm1.bc,use_local,plx \
// RUN:  -r %t.thinltonewpm1.bc,w, \
// RUN:  -r %t.thinltonewpm2.bc,use,plx \
// RUN:  -r %t.thinltonewpm2.bc,z, 2>&1 | FileCheck %s -check-prefixes=SSI,XSAFE,YSAFE

void use(int *p);

#ifdef BUILD2

int z;
__attribute__((noinline)) void use(int *p) { *p = z; }

#else

char w;
__attribute__((noinline)) void use_local(char *p) { *p = w; }

// SSI-LABEL: @fn
// SSI-LABEL: allocas uses:
int fn() {
  // XUNSAFE-DAG: [4]: full-set
  // XSAFE-DAG: [4]: [0,4)
  int x;
  use(&x);

  // YUNSAFE-DAG: [1]: full-set
  // YSAFE-DAG: [1]: [0,1)
  char y;
  use_local(&y);
  return x + y;
}

// CHECK-NOT: allocas uses:

#endif
