#pragma once
// Uniform spatial hash in CSR (compressed sparse row) form.
//
// Rebuilt from scratch every tick by counting sort:
//   pass 1: count entities per cell
//   pass 2: prefix-sum the counts into offsets
//   pass 3: scatter entity indices into a flat array
//
// Three linear passes over preallocated memory. No per-cell vectors, no
// allocation, no pointer chasing, and the resulting index array is laid out so
// that an area-of-interest query reads consecutive cells as consecutive memory.
// The alternative -- a vector per cell -- allocates on the tick path and
// scatters the working set across the heap, which is exactly the pattern that
// turns a 40us tick into a 400us one at p99.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "ge/core/assert.hpp"
#include "ge/core/platform.hpp"

namespace ge::world {

struct Bounds {
    float min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    float width()  const noexcept { return max_x - min_x; }
    float height() const noexcept { return max_y - min_y; }
};

class SpatialGrid {
public:
    void configure(Bounds b, float cell_size, std::size_t max_entities) {
        bounds_    = b;
        cell_size_ = cell_size;
        inv_cell_  = 1.0f / cell_size;
        cols_ = std::max(1, int(std::ceil(b.width()  / cell_size)));
        rows_ = std::max(1, int(std::ceil(b.height() / cell_size)));
        counts_.assign(std::size_t(cols_ * rows_) + 1, 0);
        offsets_.assign(std::size_t(cols_ * rows_) + 1, 0);
        indices_.assign(max_entities, 0);
        cell_of_.assign(max_entities, 0);
    }

    void rebuild(const float* GE_RESTRICT xs, const float* GE_RESTRICT ys, std::size_t n) noexcept {
        GE_ASSERT(n <= indices_.size());
        std::fill(counts_.begin(), counts_.end(), 0);

        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t c = cell_index(xs[i], ys[i]);
            cell_of_[i] = c;
            ++counts_[c];
        }
        std::uint32_t acc = 0;
        for (std::size_t c = 0; c < counts_.size(); ++c) {
            offsets_[c] = acc;
            acc += counts_[c];
        }
        // Reuse counts_ as the per-cell write cursor; a second array here would
        // be one more stream competing for L1 for no benefit.
        std::fill(counts_.begin(), counts_.end(), 0);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t c = cell_of_[i];
            indices_[offsets_[c] + counts_[c]++] = std::uint32_t(i);
        }
        count_ = n;
    }

    // Entity indices in the cells overlapping the query AABB. `visit` is called
    // with a span per cell row-segment, so the caller sees contiguous memory.
    template <typename Fn>
    void query_aabb(float cx, float cy, float radius, Fn&& visit) const noexcept {
        const int c0 = clamp_col(int(std::floor((cx - radius - bounds_.min_x) * inv_cell_)));
        const int c1 = clamp_col(int(std::floor((cx + radius - bounds_.min_x) * inv_cell_)));
        const int r0 = clamp_row(int(std::floor((cy - radius - bounds_.min_y) * inv_cell_)));
        const int r1 = clamp_row(int(std::floor((cy + radius - bounds_.min_y) * inv_cell_)));

        for (int r = r0; r <= r1; ++r) {
            // Cells in one row are contiguous in `indices_`, so the whole row
            // segment is a single span rather than one call per cell.
            const std::size_t first = std::size_t(r * cols_ + c0);
            const std::size_t last  = std::size_t(r * cols_ + c1);
            const std::uint32_t begin = offsets_[first];
            const std::uint32_t end   = offsets_[last] + counts_[last];
            if (end > begin) visit(std::span<const std::uint32_t>(&indices_[begin], end - begin));
        }
    }

    std::uint32_t cell_index(float x, float y) const noexcept {
        const int c = clamp_col(int((x - bounds_.min_x) * inv_cell_));
        const int r = clamp_row(int((y - bounds_.min_y) * inv_cell_));
        return std::uint32_t(r * cols_ + c);
    }

    int  cols() const noexcept { return cols_; }
    int  rows() const noexcept { return rows_; }
    std::size_t entities() const noexcept { return count_; }
    std::size_t bytes() const noexcept {
        return (counts_.capacity() + offsets_.capacity() + indices_.capacity() + cell_of_.capacity())
               * sizeof(std::uint32_t);
    }

private:
    int clamp_col(int c) const noexcept { return std::clamp(c, 0, cols_ - 1); }
    int clamp_row(int r) const noexcept { return std::clamp(r, 0, rows_ - 1); }

    Bounds bounds_{};
    float  cell_size_ = 1.0f;
    float  inv_cell_  = 1.0f;
    int    cols_ = 1, rows_ = 1;
    std::size_t count_ = 0;

    std::vector<std::uint32_t> counts_;
    std::vector<std::uint32_t> offsets_;
    std::vector<std::uint32_t> indices_;
    std::vector<std::uint32_t> cell_of_;
};

} // namespace ge::world
