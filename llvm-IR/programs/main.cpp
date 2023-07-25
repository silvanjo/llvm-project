#include <stdio.h>

int main() {
    int buffer1[10];
    for (int i = 0; i < 10; ++i) {
        buffer1[i] = i;
    }

    char buffer2[5] = "test";
    printf("Buffer2: %s\n", buffer2);

    float buffer3[15];
    for (int i = 0; i < 15; ++i) {
        buffer3[i] = (float)i / 2;
    }

    // Out of bounds access
    buffer1[10] = 0;

    return 0;
}