; ModuleID = '../llvm-IR/add.ll'
source_filename = "../llvm-IR/add.ll"

@0 = private unnamed_addr constant [19 x i8] c"Out of Bounds: %d\0A\00", align 1
@1 = private unnamed_addr constant [19 x i8] c"Out of Bounds: %d\0A\00", align 1

define i32 @add(i32 %x, i32 %y) {
check_argument_bound:
  %0 = icmp sge i32 %x, 10
  br i1 %0, label %then, label %check_argument_bound1

then:                                             ; preds = %check_argument_bound
  %1 = call i32 (ptr, ...) @printf(ptr @0, i32 %x)
  ret i32 -1

check_argument_bound1:                            ; preds = %check_argument_bound
  %2 = icmp sge i32 %y, 10
  br i1 %2, label %then2, label %check_argument_bound3

then2:                                            ; preds = %check_argument_bound1
  %3 = call i32 (ptr, ...) @printf(ptr @1, i32 %y)
  ret i32 -1

check_argument_bound3:                            ; preds = %check_argument_bound1
  br label %entry

entry:                                            ; preds = %check_argument_bound3
  %sum = add i32 %x, %y
  ret i32 %sum
}

define i32 @main() {
entry:
  %eight = call i32 @add(i32 15, i32 3)
  %0 = call i32 @add(i32 5, i32 14)
  %1 = call i32 @add(i32 20, i32 0)
  %2 = call i32 @add(i32 100, i32 200)
  ret i32 %eight
}

declare i32 @printf(ptr, ...)

declare i32 @printf.1(ptr, ...)
