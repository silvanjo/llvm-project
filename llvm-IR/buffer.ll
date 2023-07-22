; ModuleID = '../llvm-IR/BufferAccessTest.ll'
source_filename = "../llvm-IR/BufferAccessTest.ll"

@.str = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1

define void @modifyBuffer(ptr %buffer, i32 %size) {
entry:
  %0 = alloca i32, align 4
  store i32 0, ptr %0, align 4
  br label %for.cond

for.cond:                                         ; preds = %for.inc, %entry
  %1 = load i32, ptr %0, align 4
  %2 = icmp slt i32 %1, %size
  br i1 %2, label %for.body, label %for.end

for.body:                                         ; preds = %for.cond
  %3 = load i32, ptr %0, align 4
  %4 = getelementptr inbounds i32, ptr %buffer, i32 %3
  %5 = load i32, ptr %0, align 4
  store i32 %5, ptr %4, align 4
  br label %for.inc

for.inc:                                          ; preds = %for.body
  %6 = load i32, ptr %0, align 4
  %7 = add nsw i32 %6, 1
  store i32 %7, ptr %0, align 4
  br label %for.cond

for.end:                                          ; preds = %for.cond
  ret void
}

define i32 @main() {
entry:
  %buffer = alloca [10 x i32], align 16
  %0 = bitcast ptr %buffer to ptr
  call void @modifyBuffer(ptr %0, i32 10)
  %1 = getelementptr inbounds [10 x i32], ptr %buffer, i32 0, i32 5
  %2 = load i32, ptr %1, align 4
  %3 = call i32 (ptr, ...) @printf(ptr @.str, i32 %2)
  ret i32 0
}

declare i32 @printf(ptr, ...)
