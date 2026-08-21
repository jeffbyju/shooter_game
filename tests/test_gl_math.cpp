// The client's linear algebra. These live in an app header rather than a
// library, and until now nothing tested them -- which is how a mirrored view
// basis and a wrong normal transform both shipped. Both bugs share a shape:
// every cheap check passes and only one specific invariant catches them.
#include "test.hpp"

#include "../apps/shooter_client/gl_math.hpp"

#include <cmath>
#include <cstdint>

using namespace ge::client;

namespace {

// Column-major: element [c*3+r] is column c, row r.
void mul3(const float* m, const float* v, float* out) noexcept {
    for (int r = 0; r < 3; ++r)
        out[r] = m[0 * 3 + r] * v[0] + m[1 * 3 + r] * v[1] + m[2 * 3 + r] * v[2];
}
// The upper-left 3x3 of a 4x4, applied to a direction.
void mul3of4(const Mat4& m, const float* v, float* out) noexcept {
    for (int r = 0; r < 3; ++r)
        out[r] = m.m[0 * 4 + r] * v[0] + m.m[1 * 4 + r] * v[1] + m.m[2 * 4 + r] * v[2];
}
float dot3(const float* a, const float* b) noexcept {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
void normalise3(float* v) noexcept {
    const float l = std::sqrt(dot3(v, v));
    if (l > 0.0f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

// A cheap deterministic generator: a seeded test reproduces exactly, and
// nothing here should depend on the clock.
struct Rng {
    std::uint64_t s;
    float next(float lo, float hi) noexcept {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const float u = float((s >> 40) & 0xFFFFFF) / float(0x1000000);
        return lo + u * (hi - lo);
    }
};

} // namespace

// The defining property of a normal matrix: a direction perpendicular to the
// surface before the transform is still perpendicular to it after. Tangents go
// through the model matrix, the normal goes through the normal matrix, and
// their dot product must stay zero.
GE_TEST(normal_matrix_keeps_normals_perpendicular_to_transformed_tangents) {
    Rng rng{0x9E3779B9u};
    float worst = 0.0f;

    for (int trial = 0; trial < 4096; ++trial) {
        // Deliberately non-uniform scale with a rotation: that is the case the
        // old mat3(u_model) could not represent.
        const Mat4 m = translate_rotate_y_scale(
            rng.next(-5, 5), rng.next(-5, 5), rng.next(-5, 5), rng.next(-3.2f, 3.2f),
            rng.next(0.2f, 3.0f), rng.next(0.2f, 3.0f), rng.next(0.2f, 3.0f));
        const Mat3 nm = normal_matrix(m);

        float t1[3] = {rng.next(-1, 1), rng.next(-1, 1), rng.next(-1, 1)};
        float t2[3] = {rng.next(-1, 1), rng.next(-1, 1), rng.next(-1, 1)};
        float n[3]  = {t1[1] * t2[2] - t1[2] * t2[1],
                       t1[2] * t2[0] - t1[0] * t2[2],
                       t1[0] * t2[1] - t1[1] * t2[0]};
        if (std::sqrt(dot3(n, n)) < 1e-2f) continue;   // tangents near-parallel
        normalise3(n);

        float T1[3], T2[3], N[3];
        mul3of4(m, t1, T1);
        mul3of4(m, t2, T2);
        mul3(nm.m, n, N);
        normalise3(T1); normalise3(T2); normalise3(N);

        worst = std::fmax(worst, std::fabs(dot3(N, T1)));
        worst = std::fmax(worst, std::fabs(dot3(N, T2)));
    }
    CHECK(worst < 1e-3f);
}

// The trap that made the first attempt at normal_matrix pass its own test: the
// inverse and the inverse TRANSPOSE are the same matrix when the input is
// diagonal, so a cofactor table written in rows instead of columns looks
// perfect under any axis-aligned scale. Only an off-diagonal input separates
// them, which is what this uses.
GE_TEST(normal_matrix_is_the_inverse_transpose_not_the_inverse) {
    const Mat4 m = translate_rotate_y_scale(0, 0, 0, 0.7f, 0.62f, 0.72f, 0.18f);
    const Mat3 nm = normal_matrix(m);

    // transpose(M3) * N must be the identity, because N = (M3^-1)^T.
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) {
            float s = 0.0f;
            for (int k = 0; k < 3; ++k) s += m.m[r * 4 + k] * nm.m[c * 3 + k];
            CHECK(std::fabs(s - (r == c ? 1.0f : 0.0f)) < 1e-4f);
        }
}

// A zero scale has no inverse. Returning NaN would fill the screen with it, so
// the degenerate case falls back to the 3x3 and keeps rendering.
GE_TEST(normal_matrix_survives_a_degenerate_model_matrix) {
    const Mat3 nm = normal_matrix(translate_scale(1, 2, 3, 1.0f, 0.0f, 1.0f));
    for (int i = 0; i < 9; ++i) CHECK(std::isfinite(nm.m[i]));
}

// For a mesh whose normals point along the scale axes -- which is every cube in
// this game -- M = R*S scales each normal by a positive constant and normalize()
// removes it. So the old shader was, on THIS geometry, exactly right. Recorded
// as a test so the claim is checked rather than asserted: the fix is insurance
// against future geometry, not a repair of something visibly broken.
GE_TEST(axis_aligned_normals_are_unaffected_by_the_normal_matrix_fix) {
    const float axes[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    const Mat4 models[] = {
        translate_scale(0, 0, 0, 0.8f, 1.8f, 0.8f),                       // player body
        translate_scale(0, 0, 0, 0.10f, 6.0f, 0.10f),                     // powerup beam
        translate_rotate_y_scale(0, 0, 0, 0.9f, 0.62f, 0.72f, 0.18f),     // jetpack plate
    };
    for (const Mat4& m : models) {
        const Mat3 nm = normal_matrix(m);
        for (const auto& a : axes) {
            float correct[3], naive[3];
            mul3(nm.m, a, correct);
            mul3of4(m, a, naive);
            normalise3(correct); normalise3(naive);
            CHECK(std::fabs(dot3(correct, naive) - 1.0f) < 1e-3f);
        }
    }
}

// The handedness invariant from the comment in view_from(). A reflected basis
// is still orthonormal, so unit length and perpendicularity both pass on a
// mirrored camera; the determinant is the only test that separates them.
GE_TEST(view_basis_is_a_rotation_and_never_a_reflection) {
    Rng rng{0xA5A5A5A5u};
    for (int trial = 0; trial < 512; ++trial) {
        const float yaw   = rng.next(-3.2f, 3.2f);
        const float pitch = rng.next(-1.5f, 1.5f);
        const Mat4 v = view_from(rng.next(-9, 9), rng.next(-9, 9), rng.next(-9, 9),
                                 yaw, pitch);

        // Rows 0,1,2 of the view rotation are right, up and -forward: the
        // matrix the renderer actually multiplies by, not a triple assembled
        // for the occasion. det must be +1.
        const float r0[3] = {v.m[0], v.m[4], v.m[8]};
        const float r1[3] = {v.m[1], v.m[5], v.m[9]};
        const float r2[3] = {v.m[2], v.m[6], v.m[10]};

        const float det = r0[0] * (r1[1] * r2[2] - r1[2] * r2[1])
                        - r0[1] * (r1[0] * r2[2] - r1[2] * r2[0])
                        + r0[2] * (r1[0] * r2[1] - r1[1] * r2[0]);
        CHECK(std::fabs(det - 1.0f) < 1e-3f);

        // Orthonormal too -- but note these pass for a MIRRORED basis as well,
        // which is the whole point of also checking the determinant.
        CHECK(std::fabs(dot3(r0, r0) - 1.0f) < 1e-3f);
        CHECK(std::fabs(dot3(r1, r1) - 1.0f) < 1e-3f);
        CHECK(std::fabs(dot3(r2, r2) - 1.0f) < 1e-3f);
        CHECK(std::fabs(dot3(r0, r1)) < 1e-3f);
        CHECK(std::fabs(dot3(r0, r2)) < 1e-3f);
        CHECK(std::fabs(dot3(r1, r2)) < 1e-3f);
    }
}
