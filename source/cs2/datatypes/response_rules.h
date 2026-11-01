#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Response dictionaries, hash tables and inline vectors have distinct native layouts.
// These views borrow engine storage and are read on the game thread under the level lock.
namespace ResponseRules
{
    inline constexpr unsigned BucketCount = 0x402;

    struct Tree
    {
        unsigned slots;
        unsigned capacityFlags;
        uint8_t* data;
        unsigned unknown010;
        unsigned count;
        uint8_t pad_18[0x10];

        unsigned Capacity() const
        {
            return capacityFlags & 0x7fffffff;
        }
    };

    struct NarrowTree
    {
        uint8_t pad_00[0x8];
        std::uint16_t slots;
        std::uint16_t capacityFlags;
        uint8_t pad_0C[0x4];
        uint8_t* data;
        std::uint16_t unknown018;
        std::uint16_t count;
        uint8_t pad_1C[0x4];

        unsigned Capacity() const
        {
            return capacityFlags & 0x7fff;
        }
    };

    template <unsigned N>
    struct InlineVector
    {
        unsigned count;
        unsigned capacityFlags;

        union
        {
            unsigned* heap;
            unsigned local[N];
        } storage;

        unsigned Capacity() const
        {
            return capacityFlags & 0x7fffffff;
        }

        const unsigned* Data() const
        {
            return Capacity() > N ? storage.heap : storage.local;
        }
    };

    struct Rule
    {
        InlineVector<6> criteria;
        InlineVector<2> groups;
    };

    struct RuleNode
    {
        uint8_t links[0x10];
        Rule* rule;
    };

    struct RuleBucket
    {
        NarrowTree tree;
        uint8_t pad_20[0x20];
    };

    struct PendingEntry
    {
        uint8_t pad_00[0x10];
        Rule* rule;
    };

    struct PendingList
    {
        unsigned count;
        uint8_t pad_04[0x4];
        PendingEntry* data;
        unsigned capacity;
    };

    struct Root
    {
        uint8_t pad_00[0x20];
        Tree groups;
        Tree criteria;
        uint8_t pad_70[0x20];
        RuleBucket buckets[1];

        // Native allocation has BucketCount dictionaries followed by a spare bucket.
        const PendingList* Pending(unsigned count) const
        {
            return reinterpret_cast<const PendingList*>(buckets + count + 1);
        }
    };

    struct CriterionNode
    {
        uint8_t links[0x18];
        std::uint16_t symbol;
        uint8_t pad_1A[0x6];
        const char* expression;
        std::uint16_t weight;
        uint8_t pad_2A[0x2];
        uint8_t limits[0x8];
        std::uint8_t matcher;
        uint8_t pad_35[0x3];

        std::uint64_t Limits() const
        {
            std::uint64_t value;
            std::memcpy(&value, limits, sizeof(value));
            return value;
        }
    };

    struct Response
    {
        uint8_t pad_00[0x3F];
        std::uint8_t type;
        const char* text;
        uint8_t pad_48[0x8];
    };

    struct Group
    {
        std::uint16_t count;
        std::uint16_t capacityFlags;
        uint8_t pad_04[0x4];
        Response* entries;

        unsigned Capacity() const
        {
            return capacityFlags & 0x7fff;
        }
    };

    struct GroupNode
    {
        uint8_t links[0x10];
        const char* name;
        Group* group;
    };

    struct WheelRow
    {
        const char* token;
        const char* conceptName;
        uint8_t pad_10[0x10];
    };
}
