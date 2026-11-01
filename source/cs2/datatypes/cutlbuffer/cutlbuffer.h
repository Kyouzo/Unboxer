#pragma once

class CUtlBuffer
{
public:
    char _pad[0x80];

    CUtlBuffer(int a1 = 0, int size = 0, int a3 = 0);

    [[nodiscard]] void* DetachMemory();
};