#pragma once
// Just enough linear algebra for a first-person camera.
//
// Hand-rolled rather than pulled in: four functions are needed, all of them
// textbook, and a matrix library would be a second third-party dependency
// after SDL3 for no benefit. Column-major, matching what glUniformMatrix4fv
// expects with transpose = GL_FALSE.
#include <cmath>

namespace ge::client {

struct Mat4 { float m[16]; };
struct Mat3 { float m[9]; };    // column-major, for glUniformMatrix3fv

inline Mat4 identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

inline Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

inline Mat4 perspective(float fov_y_rad, float aspect, float znear, float zfar) {
    const float f = 1.0f / std::tan(fov_y_rad * 0.5f);
    Mat4 r{};
    r.m[0]  = f / aspect;
    r.m[5]  = f;
    r.m[10] = (zfar + znear) / (znear - zfar);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * zfar * znear) / (znear - zfar);
    return r;
}

// Right-handed look-at built directly from yaw/pitch, so it matches the
// server's convention exactly: forward is +z rotated by yaw, y is up.
inline Mat4 view_from(float eye_x, float eye_y, float eye_z, float yaw, float pitch) {
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float fx = std::sin(yaw) * cp, fy = sp, fz = std::cos(yaw) * cp;
    // right = cross(forward, worldUp);  up = cross(right, forward).
    //
    // Validate the rule against the standard GL camera, where the answer is not
    // in doubt: forward=-z, up=+y, right=+x, and (0,0,-1)x(0,1,0) = (+1,0,0).
    // So right = forward x up. Our forward is +z, which makes right -x, NOT +x.
    //
    // This previously used right=+x with up=cross(forward,right). That also
    // yields up=+y, so it looked correct and every perpendicularity check
    // passed -- but det(right, up, -forward) came out -1 instead of +1, meaning
    // the view basis was a rotation PLUS a reflection and the world rendered
    // mirrored. Nothing in this arena is handed (symmetric cubes, symmetric
    // grid, no text), so the mirror was self-consistent and invisible; it would
    // have appeared the moment anything asymmetric was drawn.
    const float rx = -std::cos(yaw), ry = 0.0f, rz = std::sin(yaw);
    const float ux = ry * fz - rz * fy;
    const float uy = rz * fx - rx * fz;
    const float uz = rx * fy - ry * fx;

    Mat4 r = identity();
    r.m[0] = rx;  r.m[4] = ry;  r.m[8]  = rz;
    r.m[1] = ux;  r.m[5] = uy;  r.m[9]  = uz;
    r.m[2] = -fx; r.m[6] = -fy; r.m[10] = -fz;
    r.m[12] = -(rx * eye_x + ry * eye_y + rz * eye_z);
    r.m[13] = -(ux * eye_x + uy * eye_y + uz * eye_z);
    r.m[14] =  (fx * eye_x + fy * eye_y + fz * eye_z);
    return r;
}

// Translate + scale with a spin about Y. Used for powerups, which rotate so
// they read as collectable objects rather than as scenery -- a static cube on
// the floor of a shooter looks like a crate.
//
// Column-major, and the rotation is applied BEFORE the scale in the maths
// below (the scale factors multiply into the rotated basis), which for a
// uniform-in-xz scale is the same thing either way.
inline Mat4 translate_rotate_y_scale(float x, float y, float z, float angle,
                                     float sx, float sy, float sz) {
    const float c = std::cos(angle), s = std::sin(angle);
    Mat4 r = identity();
    r.m[0]  =  c * sx; r.m[2]  = -s * sx;
    r.m[5]  =  sy;
    r.m[8]  =  s * sz; r.m[10] =  c * sz;
    r.m[12] = x; r.m[13] = y; r.m[14] = z;
    return r;
}

inline Mat4 translate_scale(float x, float y, float z, float sx, float sy, float sz) {
    Mat4 r = identity();
    r.m[0] = sx; r.m[5] = sy; r.m[10] = sz;
    r.m[12] = x; r.m[13] = y; r.m[14] = z;
    return r;
}

// The matrix a NORMAL has to be transformed by, which is not the model matrix.
//
// A tangent lying in the surface is stretched along with the surface. The
// normal is perpendicular to it and must do the opposite -- squash a cube in x
// and the normal of a slanted face tilts AWAY from x, not toward it. Applying
// the model matrix to both tilts them the same way and they stop being
// perpendicular, so the lighting is computed against a direction that is not
// the surface normal.
//
// The correct transform is the inverse transpose of the upper-left 3x3. For a
// pure rotation that equals the matrix itself (M^-1 = M^T, so (M^-1)^T = M),
// and for a UNIFORM scale it differs only by a constant that normalize()
// removes -- which is why `mat3(u_model)` survived here for so long. It is
// wrong the moment a scale is non-uniform, and the player body is scaled
// 0.8 x 1.8 x 0.8.
//
// inverse(A)^T is the cofactor matrix over the determinant. Computing it that
// way needs no division until the end and no general matrix inverse.
inline Mat3 normal_matrix(const Mat4& model) {
    // Upper-left 3x3 of the column-major 4x4: a[col][row].
    const float a00 = model.m[0], a10 = model.m[1], a20 = model.m[2];
    const float a01 = model.m[4], a11 = model.m[5], a21 = model.m[6];
    const float a02 = model.m[8], a12 = model.m[9], a22 = model.m[10];

    // Cofactors. c[i][j] is the signed minor of a[i][j], and the cofactor
    // matrix laid out this way IS the transpose of the adjugate's transpose --
    // i.e. exactly inverse-transpose, up to the determinant.
    const float c00 =  (a11 * a22 - a21 * a12);
    const float c10 = -(a01 * a22 - a21 * a02);
    const float c20 =  (a01 * a12 - a11 * a02);
    const float c01 = -(a10 * a22 - a20 * a12);
    const float c11 =  (a00 * a22 - a20 * a02);
    const float c21 = -(a00 * a12 - a10 * a02);
    const float c02 =  (a10 * a21 - a20 * a11);
    const float c12 = -(a00 * a21 - a20 * a01);
    const float c22 =  (a00 * a11 - a10 * a01);

    const float det = a00 * c00 + a10 * c10 + a20 * c20;
    // A degenerate model matrix (a zero scale on some axis) has no inverse.
    // Falling back to the 3x3 keeps the frame rendering instead of filling the
    // screen with NaN, which is the right failure for a renderer.
    if (det > -1e-20f && det < 1e-20f)
        return Mat3{{a00, a10, a20, a01, a11, a21, a02, a12, a22}};

    // Column-major, so each triple below is a COLUMN: entry [c*3+r] must be
    // cofactor(r, c). Writing the rows here instead yields the inverse rather
    // than the inverse transpose -- and the two are identical for a diagonal
    // matrix, so a test using only axis-aligned scales passes either way. This
    // was wrong first time round for exactly that reason.
    const float inv = 1.0f / det;
    return Mat3{{c00 * inv, c10 * inv, c20 * inv,
                 c01 * inv, c11 * inv, c21 * inv,
                 c02 * inv, c12 * inv, c22 * inv}};
}

} // namespace ge::client
