#pragma once
// Just enough linear algebra for a first-person camera. Column-major, which is
// what glUniformMatrix4fv wants with transpose = GL_FALSE.
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

// Right-handed, built from yaw/pitch so it matches the server: forward is +z
// rotated by yaw, y is up.
inline Mat4 view_from(float eye_x, float eye_y, float eye_z, float yaw, float pitch) {
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float fx = std::sin(yaw) * cp, fy = sp, fz = std::cos(yaw) * cp;
    // right = forward x up. Check it on the standard GL camera, where the
    // answer is not in doubt: (0,0,-1)x(0,1,0) = (+1,0,0). Our forward is +z,
    // so right is -x.
    //
    // This used right=+x before. up still came out +y and every
    // perpendicularity check passed, but det(right,up,-forward) was -1 -- a
    // reflection. Nothing here is handed, so the mirrored world was invisible.
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

// Translate + scale with a spin about Y, for powerups: a static cube on the
// floor reads as scenery. Rotation applies before the scale.
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

// Normals transform by the inverse transpose of the upper-left 3x3, not by the
// model matrix: squash a cube in x and a slanted face's normal tilts away from
// x while its tangent tilts toward it. Equal for pure rotation, and off only by
// a constant normalize() removes for uniform scale, which is why mat3(u_model)
// survived so long here.
//
// inverse(A)^T is the cofactor matrix over the determinant -- no general
// inverse needed, and only one division.
inline Mat3 normal_matrix(const Mat4& model) {
    // Upper-left 3x3 of the column-major 4x4: a[col][row].
    const float a00 = model.m[0], a10 = model.m[1], a20 = model.m[2];
    const float a01 = model.m[4], a11 = model.m[5], a21 = model.m[6];
    const float a02 = model.m[8], a12 = model.m[9], a22 = model.m[10];

    // c[i][j] is the signed minor of a[i][j].
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
    // No inverse for a zero scale. Fall back to the 3x3 rather than fill the
    // screen with NaN.
    if (det > -1e-20f && det < 1e-20f)
        return Mat3{{a00, a10, a20, a01, a11, a21, a02, a12, a22}};

    // Column-major: [c*3+r] must be cofactor(r,c). Writing rows here gives the
    // inverse instead, which is the same matrix for a diagonal input -- so an
    // axis-aligned test passes either way. It was wrong this way first.
    const float inv = 1.0f / det;
    return Mat3{{c00 * inv, c10 * inv, c20 * inv,
                 c01 * inv, c11 * inv, c21 * inv,
                 c02 * inv, c12 * inv, c22 * inv}};
}

} // namespace ge::client
