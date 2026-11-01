#pragma once

#include "../../utils/memory/vfunc/vfunc.h"

class IFileSystem
{
public:
    // Verified against VFileSystem017 / CFileSystem_Stdio on September 23.
    // The basic wrappers dispatch to OpenEx (75) and ReadEx (76) themselves.
    [[nodiscard]] void* Open(const char* fileName, const char* options = "rb", const char* pathID = "GAME")
    {
        return M::CallVFunc<void*, 13U>(this, fileName, options, pathID);
    }

    [[nodiscard]] int Read(void* destination, int size, void* handle)
    {
        return M::CallVFunc<int, 11U>(this, destination, size, handle);
    }

    [[nodiscard]] std::uint64_t Size(void* handle)
    {
        return M::CallVFunc<std::uint64_t, 18U>(this, handle);
    }

    void Close(void* handle)
    {
        M::CallVFunc<void, 14U>(this, handle);
    }

    [[nodiscard]] bool Exists(const char* fileName, const char* pathID = "GAME")
    {
        return M::CallVFunc<bool, 21U>(this, fileName, pathID);
    }
};
