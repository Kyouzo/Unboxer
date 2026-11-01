#include "cutlbuffer.h"
#include "../../../chadware2/interfaces/interfaces.h"

CUtlBuffer::CUtlBuffer(int a1, int size, int a3)
{
    if (I::ConstructUtlBuffer)
        I::ConstructUtlBuffer(this, a1, size, a3);
}

void* CUtlBuffer::DetachMemory()
{
    return I::DetachUtlBufferMemory ? I::DetachUtlBufferMemory(this) : nullptr;
}
