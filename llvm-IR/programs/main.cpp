#include <iostream>

void testBuffer1() {
    int arr1[10];            // Static buffer allocation
    for(int i = 0; i < 10; i++) {
        arr1[i] = i;
    }

    int index = 7;
    std::cout << arr1[index] << std::endl;  // Access within bounds

    int *dynamicArr = new int[20];          // Dynamic buffer allocation (Note: your pass might not catch this one as it is heap allocated)
    dynamicArr[19] = 50;
    delete[] dynamicArr;
}

void testBuffer2() {
    char buffer[5];
    buffer[4] = 'A';

    int index = 3;
    std::cout << buffer[index] << std::endl;  // Access within bounds
}

void testOutOfBounds() {
    int arr2[8];
    arr2[10] = 100;          // Out of bounds access
}

int main() {
    testBuffer1();
    testBuffer2();
    testOutOfBounds();

    return 0;
}
