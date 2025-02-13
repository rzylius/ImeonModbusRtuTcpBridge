#include "config.h"
#include <Arduino.h>
#include <IPAddress.h>

// IP address to send battery info to modbusTCP server
IPAddress mbBatDestination(10, 0, 20, 220); 

// Define the array in one translation unit (source file)
const RegisterRange predefinedRanges[] = {
    {0x100, 30},  // 256 AC grid
    {0x200, 22},  // 512 AC output
    {0x300, 4},   // 768 battery
    {0x400, 16},  // 1024 PV
    {0x503, 6},   // 1283 errors and warnings
    // {0x600, 3}, // 1536 smartmeter, I read smartmeter real time from other device
    {0x1000, 5},  // 4096 lcd and time
    {0x1100, 2},  // 4352 max grid injection
    {0x1300, 18}, // 4864 battery soc, limitations and management
    {0x1323, 1},  // 4899 imeon running mode  smartgrid backup etc
    {0x1405, 8}   // 5125 battery voltages
};

// Define the count of elements
const int rangeCount = sizeof(predefinedRanges) / sizeof(predefinedRanges[0]);