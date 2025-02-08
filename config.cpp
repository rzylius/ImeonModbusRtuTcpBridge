#include "config.h"

// Define the array in one translation unit (source file)
const RegisterRange predefinedRanges[] = {
    {256, 30},
    {512, 22},
    {768, 4},
    {1024, 16},
    {1283, 6},
    {4096, 5},
    {4352, 2},
    {4864, 18},
    {4899, 1},
    {5125, 8}
};

// Define the count of elements
const int rangeCount = sizeof(predefinedRanges) / sizeof(predefinedRanges[0]);
