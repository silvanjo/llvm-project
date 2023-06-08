#include <iostream>

int arr1[10] = { 0 };
int* arr2 = (int*) malloc(10 * sizeof(int));

int SumArray(int* arr, int size)
{
    int sum = 0;
    for (int i = 0; i < size; i++)
    {
        sum += arr[i];
    }

    return sum;
}

int main()
{
    int sum1 = SumArray(arr1, 10);
    int sum2 = SumArray(arr2, 10);

    std::cout << "Sum1: " << sum1 << std::endl;
    std::cout << "Sum2: " << sum2 << std::endl;

    return 0;
}