#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Read-only release MSVC string and protobuf views. The game owns their allocation
// and lifetime. Never construct or destroy them through this DLL's C++ runtime.
struct NativeMessageString
{
    union
    {
        const char* heap;
        char local[16];
    } storage;
    std::size_t length;
    std::size_t capacity;

    bool Copy(char* out, std::size_t size) const
    {
        if (length >= size || length > capacity)
            return false;

        const auto* text = capacity > 15 ? storage.heap : storage.local;
        std::memcpy(out, text, length);
        out[length] = '\0';
        return true;
    }
};

// Protobuf can retain allocated string objects after removing live parameters.
// This Rep allocation count is not the message's current parameter count.
struct RadioArguments
{
    int allocatedCount;
    uint8_t pad_04[0x4];
    const NativeMessageString* entries[1];
};

struct RadioTextMessage
{
    uint8_t pad_00[0x50];
    int argumentCount;
    int argumentCapacity;
    const RadioArguments* arguments;
    std::uintptr_t taggedFormat;
    int destination;
    int entityIndex;

    const NativeMessageString* Format() const
    {
        return reinterpret_cast<const NativeMessageString*>(taggedFormat & ~std::uintptr_t{3});
    }
};

static_assert(offsetof(RadioArguments, entries) == 0x8);
static_assert(offsetof(RadioTextMessage, argumentCount) == 0x50);
static_assert(offsetof(RadioTextMessage, argumentCapacity) == 0x54);
static_assert(offsetof(RadioTextMessage, arguments) == 0x58);
static_assert(offsetof(RadioTextMessage, taggedFormat) == 0x60);
static_assert(offsetof(RadioTextMessage, destination) == 0x68);
static_assert(offsetof(RadioTextMessage, entityIndex) == 0x6C);
