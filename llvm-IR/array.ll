
@0 = private unnamed_addr constant [11 x i8] c"Index: %d\0A\00", align 1

@array = global [10 x i32] [i32 0, i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 7, i32 8, i32 9]


define void @print_array() {
  %i = alloca i32
  store i32 0, i32* %i
  br label %loop

loop:
  %index = load i32, i32* %i
  %cmp = icmp slt i32 %index, 10
  br i1 %cmp, label %body, label %end

body:
  %ptr = getelementptr [10 x i32], [10 x i32]* @array, i32 0, i32 %index
  %element = load i32, i32* %ptr
  call i32 (ptr, ...) @printf(ptr @0, i32 %element)
  %next_index = add i32 %index, 1
  store i32 %next_index, i32* %i
  br label %loop

end:
  ret void
}

define i32 @main() {
  call void @print_array()
  ret i32 0
}

declare i32 @printf(ptr, ...)