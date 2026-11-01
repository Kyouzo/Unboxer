#pragma once

#include <cassert>

template <class T>
class c_utl_vector
{
public:
    [[nodiscard]] int count() const { return m_size; }
    [[nodiscard]] T& element(int index) { assert(index >= 0 && index < m_size); return m_elements[index]; }
    [[nodiscard]] const T& element(int index) const { assert(index >= 0 && index < m_size); return m_elements[index]; }

    int m_size = 0;
    T* m_elements = nullptr;
};
