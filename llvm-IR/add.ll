
define i32 @add(i32 %x, i32 %y) {
entry:
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