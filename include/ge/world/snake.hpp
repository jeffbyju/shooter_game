#pragma once
// Snake storage.
//
// A snake is a head that moves at constant speed along a heading, trailing a
// body made of the positions its head recently occupied. Players steer by
// changing the heading; they cannot stop, which is what makes the game a game.
//
// Layout follows the same rule as everything else on the tick path: the fields
// the hot loops touch live in their own arrays, and the body points live in one
// flat slab with a fixed stride, so advancing every snake is a linear walk with
// no allocation and no pointer chasing.
//
// The body is a per-snake ring buffer. Moving forward is one store plus an index
// bump -- the tail is not shifted, it is simply left behind by the window.
#include <cmath>
#include <cstdint>
#include <vector>

#include "ge/core/platform.hpp"
#include "ge/proto/wire.hpp"

namespace ge::world {

// Where to send this snake's view of the world. Kept out of the hot arrays.
struct ClientLink {
    std::uint32_t session   = 0;
    std::uint32_t conn_id   = 0;   // index in the owning I/O thread's connection slab
    std::uint16_t io_thread = 0;
    std::uint16_t flags     = 0;
};

inline constexpr std::uint16_t kStartLength   = 8;
inline constexpr std::uint16_t kMaxLength     = std::uint16_t(proto::kMaxSnakeSegments);
// How much of a victim's length the killer keeps. Taking all of it makes the
// first kill decisive; taking half keeps the game open.
inline constexpr float         kEatFraction   = 0.5f;
inline constexpr float         kSegmentSpacing= 2.0f;    // world units between body points
inline constexpr float         kCollideRadius = 1.6f;
inline constexpr float         kBaseSpeed     = 26.0f;   // units per second
inline constexpr float         kTurnRate      = 4.5f;    // radians per second

class SnakeStore {
public:
    void reserve(std::size_t n) {
        id_.reserve(n); x_.reserve(n); y_.reserve(n);
        heading_.reserve(n); speed_.reserve(n);
        length_.reserve(n); kills_.reserve(n);
        alive_.reserve(n); respawn_at_.reserve(n);
        link_.reserve(n); last_seq_.reserve(n); last_ts_.reserve(n);
        want_heading_.reserve(n); since_drop_.reserve(n);
        body_head_.reserve(n); body_count_.reserve(n);
        body_.reserve(n * kMaxLength);
        cap_ = n;
    }

    std::uint32_t add(std::uint32_t id, float x, float y, float heading, ClientLink link) {
        const auto slot = std::uint32_t(id_.size());
        id_.push_back(id); x_.push_back(x); y_.push_back(y);
        heading_.push_back(heading); want_heading_.push_back(heading);
        speed_.push_back(kBaseSpeed);
        length_.push_back(kStartLength); kills_.push_back(0);
        alive_.push_back(1); respawn_at_.push_back(0);
        link_.push_back(link); last_seq_.push_back(0); last_ts_.push_back(0);
        since_drop_.push_back(0.0f);
        body_head_.push_back(0); body_count_.push_back(0);
        body_.resize(body_.size() + kMaxLength);
        seed_body(slot, x, y, heading);
        return slot;
    }

    // Swap-and-pop keeps every array dense. Returns the index that moved into
    // `i` so the caller can fix up its own maps.
    std::uint32_t remove(std::uint32_t i) noexcept {
        const auto last = std::uint32_t(id_.size() - 1);
        if (i != last) {
            id_[i]=id_[last]; x_[i]=x_[last]; y_[i]=y_[last];
            heading_[i]=heading_[last]; want_heading_[i]=want_heading_[last];
            speed_[i]=speed_[last]; length_[i]=length_[last]; kills_[i]=kills_[last];
            alive_[i]=alive_[last]; respawn_at_[i]=respawn_at_[last];
            link_[i]=link_[last]; last_seq_[i]=last_seq_[last]; last_ts_[i]=last_ts_[last];
            since_drop_[i]=since_drop_[last];
            body_head_[i]=body_head_[last]; body_count_[i]=body_count_[last];
            for (std::size_t k = 0; k < kMaxLength; ++k)
                body_[std::size_t(i) * kMaxLength + k] = body_[std::size_t(last) * kMaxLength + k];
        }
        id_.pop_back(); x_.pop_back(); y_.pop_back();
        heading_.pop_back(); want_heading_.pop_back(); speed_.pop_back();
        length_.pop_back(); kills_.pop_back();
        alive_.pop_back(); respawn_at_.pop_back();
        link_.pop_back(); last_seq_.pop_back(); last_ts_.pop_back();
        since_drop_.pop_back();
        body_head_.pop_back(); body_count_.pop_back();
        body_.resize(body_.size() - kMaxLength);
        return last;
    }

    // Lays the body out straight behind the head, so a fresh snake looks like a
    // snake on the very first frame rather than a dot that grows.
    void seed_body(std::uint32_t i, float x, float y, float heading) noexcept {
        const float dx = -std::cos(heading) * kSegmentSpacing;
        const float dy = -std::sin(heading) * kSegmentSpacing;
        body_count_[i] = length_[i];
        body_head_[i]  = 0;
        for (std::uint16_t k = 0; k < length_[i]; ++k)
            body_[std::size_t(i) * kMaxLength + k] =
                proto::Point2{x + dx * float(k), y + dy * float(k)};
        x_[i] = x; y_[i] = y;
    }

    // Pushes a new head point, letting the window leave the tail behind.
    GE_ALWAYS_INLINE void push_point(std::uint32_t i, float x, float y) noexcept {
        const std::uint16_t h = std::uint16_t((body_head_[i] + kMaxLength - 1) % kMaxLength);
        body_head_[i] = h;
        body_[std::size_t(i) * kMaxLength + h] = proto::Point2{x, y};
        if (body_count_[i] < length_[i]) ++body_count_[i];
        else if (body_count_[i] > length_[i]) body_count_[i] = length_[i];
    }

    // Body point k, counting from the head.
    GE_ALWAYS_INLINE proto::Point2 point(std::uint32_t i, std::uint16_t k) const noexcept {
        const std::uint16_t idx = std::uint16_t((body_head_[i] + k) % kMaxLength);
        return body_[std::size_t(i) * kMaxLength + idx];
    }

    std::size_t size() const noexcept { return id_.size(); }

    std::uint32_t* id()            noexcept { return id_.data(); }
    float*         x()             noexcept { return x_.data(); }
    float*         y()             noexcept { return y_.data(); }
    float*         heading()       noexcept { return heading_.data(); }
    float*         want_heading()  noexcept { return want_heading_.data(); }
    float*         speed()         noexcept { return speed_.data(); }
    float*         since_drop()    noexcept { return since_drop_.data(); }
    std::uint16_t* length()        noexcept { return length_.data(); }
    std::uint16_t* kills()         noexcept { return kills_.data(); }
    std::uint8_t*  alive()         noexcept { return alive_.data(); }
    std::int64_t*  respawn_at()    noexcept { return respawn_at_.data(); }
    ClientLink*    link()          noexcept { return link_.data(); }
    std::uint32_t* last_seq()      noexcept { return last_seq_.data(); }
    std::uint64_t* last_ts()       noexcept { return last_ts_.data(); }
    std::uint16_t* body_count()    noexcept { return body_count_.data(); }

    std::size_t bytes() const noexcept {
        return body_.capacity() * sizeof(proto::Point2)
             + cap_ * (sizeof(std::uint32_t) * 3 + sizeof(float) * 5
                     + sizeof(std::uint16_t) * 3 + sizeof(std::uint8_t)
                     + sizeof(std::int64_t) + sizeof(ClientLink) + sizeof(std::uint64_t));
    }

private:
    std::vector<std::uint32_t> id_;
    std::vector<float>         x_, y_, heading_, want_heading_, speed_, since_drop_;
    std::vector<std::uint16_t> length_, kills_, body_count_;
    std::vector<std::uint8_t>  alive_;
    std::vector<std::int64_t>  respawn_at_;
    std::vector<ClientLink>    link_;
    std::vector<std::uint32_t> last_seq_;
    std::vector<std::uint64_t> last_ts_;
    std::vector<std::uint16_t> body_head_;
    std::vector<proto::Point2> body_;          // [snake * kMaxLength + slot]
    std::size_t                cap_ = 0;
};

} // namespace ge::world
