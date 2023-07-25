; ModuleID = '../llvm-IR/BufferAccessTest.ll'
source_filename = "printbuffer.ll"

@.str = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@printLog = private unnamed_addr constant [26 x i8] c"Index being accessed: %d\0A\00", align 1

declare i32 @printf(ptr, ...)

define i32 @main() {
entry:
  %buffer = alloca [5 x i32], align 16
  %0 = bitcast ptr %buffer to ptr
  call void @llvm.memset.p0.i64(ptr nonnull %0, i8 0, i64 20, i1 false)
  %1 = getelementptr inbounds [5 x i32], ptr %buffer, i64 0, i64 0
  store i32 0, ptr %1, align 16
  %2 = getelementptr inbounds [5 x i32], ptr %buffer, i64 0, i64 1
  store i32 1, ptr %2, align 4
  %3 = getelementptr inbounds [5 x i32], ptr %buffer, i64 0, i64 2
  store i32 2, ptr %3, align 8
  %4 = getelementptr inbounds [5 x i32], ptr %buffer, i64 0, i64 3
  store i32 3, ptr %4, align 4
  %5 = getelementptr inbounds [5 x i32], ptr %buffer, i64 0, i64 4
  store i32 4, ptr %5, align 16
  br label %for.cond

for.cond:                                         ; preds = %for.inc, %entry
  %i.0 = phi i32 [ 0, %entry ], [ %inc, %for.inc ]
  %exitcond = icmp eq i32 %i.0, 5
  br i1 %exitcond, label %for.end, label %for.body

for.body:                                         ; preds = %for.cond
  %6 = call i32 (ptr, ...) @printf(ptr @printLog, i32 %i.0)
  %7 = getelementptr inbounds [5 x i32], ptr %buffer, i64 0, i32 %i.0
  %8 = load i32, ptr %7, align 4
  %9 = call i32 (ptr, ...) @printf(ptr @.str, i32 %8)
  br label %for.inc

for.inc:                                          ; preds = %for.body
  %inc = add nsw i32 %i.0, 1
  br label %for.cond

for.end:                                          ; preds = %for.cond
  ret i32 0
}

; Function Attrs: nocallback nofree nounwind willreturn memory(argmem: write)
declare void @llvm.memset.p0.i64(ptr nocapture writeonly, i8, i64, i1 immarg) #0

attributes #0 = { nocallback nofree nounwind willreturn memory(argmem: write) }
