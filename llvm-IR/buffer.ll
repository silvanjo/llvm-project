; ModuleID = 'printbuffer.ll'
source_filename = "printbuffer.ll"

@.str = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1

declare i32 @printf(i8*, ...)

define i32 @main() {
entry:
  %buffer = alloca [5 x i32], align 16
  %0 = bitcast [5 x i32]* %buffer to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %0, i8 0, i64 20, i1 false)
  %1 = getelementptr inbounds [5 x i32], [5 x i32]* %buffer, i64 0, i64 0
  store i32 0, i32* %1, align 16
  %2 = getelementptr inbounds [5 x i32], [5 x i32]* %buffer, i64 0, i64 1
  store i32 1, i32* %2, align 4
  %3 = getelementptr inbounds [5 x i32], [5 x i32]* %buffer, i64 0, i64 2
  store i32 2, i32* %3, align 8
  %4 = getelementptr inbounds [5 x i32], [5 x i32]* %buffer, i64 0, i64 3
  store i32 3, i32* %4, align 4
  %5 = getelementptr inbounds [5 x i32], [5 x i32]* %buffer, i64 0, i64 4
  store i32 4, i32* %5, align 16
  br label %for.cond

for.cond:                                         ; preds = %for.inc, %entry
  %i.0 = phi i32 [ 0, %entry ], [ %inc, %for.inc ]
  %exitcond = icmp eq i32 %i.0, 5
  br i1 %exitcond, label %for.end, label %for.body

for.body:                                         ; preds = %for.cond
  %6 = getelementptr inbounds [5 x i32], [5 x i32]* %buffer, i64 0, i32 %i.0
  %7 = load i32, i32* %6, align 4
  %8 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @.str, i32 0, i32 0), i32 %7)
  br label %for.inc

for.inc:                                          ; preds = %for.body
  %inc = add nsw i32 %i.0, 1
  br label %for.cond

for.end:                                          ; preds = %for.cond
  ret i32 0
}

declare void @llvm.memset.p0i8.i64(i8* nocapture writeonly, i8, i64, i1) #1

attributes #1 = { nofree nosync nounwind willreturn }