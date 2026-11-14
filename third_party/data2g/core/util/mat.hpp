// A row-major 2-D array: what numpy's (B, n) batches become. Deliberately
// minimal; anything fancier goes in the module that needs it.
#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace data2g {

template <typename T>
struct Mat {
    std::size_t rows = 0, cols = 0;
    std::vector<T> data;

    Mat() = default;
    Mat(std::size_t r, std::size_t c, T fill = T()) : rows(r), cols(c), data(r * c, fill) {}

    T* operator[](std::size_t r) { return data.data() + r * cols; }
    const T* operator[](std::size_t r) const { return data.data() + r * cols; }
    std::span<T> row(std::size_t r) { return {(*this)[r], cols}; }
    std::span<const T> row(std::size_t r) const { return {(*this)[r], cols}; }
};

}  // namespace data2g
