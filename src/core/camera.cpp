#include "camera.h"

#include <algorithm>
#include <cmath>

namespace v3d {

namespace {
constexpr float kHalfPi = 1.5707963f;

float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
}  // namespace

void Camera::basis(float r[3], float u[3], float b[3]) const {
    float cp = std::cos(pitch_), sp = std::sin(pitch_);
    float cy = std::cos(yaw_), sy = std::sin(yaw_);
    b[0] = cp * cy, b[1] = cp * sy, b[2] = sp;
    // Right stays horizontal, so the basis never degenerates at the poles.
    r[0] = -sy, r[1] = cy, r[2] = 0;
    u[0] = b[1] * r[2] - b[2] * r[1];
    u[1] = b[2] * r[0] - b[0] * r[2];
    u[2] = b[0] * r[1] - b[1] * r[0];
}

void Camera::eye(float out[3]) const {
    float r[3], u[3], b[3];
    basis(r, u, b);
    for (int i = 0; i < 3; ++i) out[i] = target_[i] + b[i] * dist_;
}

void Camera::fit(const BBox& bb, float aspect) {
    if (!bb.valid()) return;
    bounds_ = bb;
    float dx = bb.max[0] - bb.min[0], dy = bb.max[1] - bb.min[1], dz = bb.max[2] - bb.min[2];
    for (int i = 0; i < 3; ++i) target_[i] = 0.5f * (bb.min[i] + bb.max[i]);
    radius_ = std::max(0.5f * std::sqrt(dx * dx + dy * dy + dz * dz), 1e-3f);
    float halfY = kFovY * 0.5f;
    float halfX = std::atan(std::tan(halfY) * aspect);
    dist_ = radius_ / std::sin(std::min(halfX, halfY)) * 0.92f;
}

void Camera::setPreset(ViewPreset p, float aspect) {
    switch (p) {
        case ViewPreset::Front: yaw_ = -kHalfPi, pitch_ = 0; break;
        case ViewPreset::Back: yaw_ = kHalfPi, pitch_ = 0; break;
        case ViewPreset::Left: yaw_ = 2 * kHalfPi, pitch_ = 0; break;
        case ViewPreset::Right: yaw_ = 0, pitch_ = 0; break;
        case ViewPreset::Top: yaw_ = -kHalfPi, pitch_ = kHalfPi; break;
        case ViewPreset::Bottom: yaw_ = -kHalfPi, pitch_ = -kHalfPi; break;
        case ViewPreset::Iso: yaw_ = kIsoYaw, pitch_ = kIsoPitch; break;
    }
    fit(bounds_, aspect);
}

void Camera::orbit(float dx, float dy) {
    yaw_ -= dx * 0.008f;
    pitch_ = std::clamp(pitch_ + dy * 0.008f, -kHalfPi, kHalfPi);
}

void Camera::pan(float dx, float dy, float viewportHeight) {
    float r[3], u[3], b[3];
    basis(r, u, b);
    float wpp = 2.0f * dist_ * std::tan(kFovY * 0.5f) / std::max(viewportHeight, 1.0f);
    for (int i = 0; i < 3; ++i) target_[i] += (-r[i] * dx + u[i] * dy) * wpp;
}

void Camera::zoom(float factor, float ndcX, float ndcY, float aspect) {
    // Keep the point under the cursor (on the focal plane) fixed on screen.
    float r[3], u[3], b[3], e[3];
    basis(r, u, b);
    eye(e);
    float th = std::tan(kFovY * 0.5f);
    float p[3];
    for (int i = 0; i < 3; ++i) p[i] = e[i] + dist_ * (-b[i] + r[i] * ndcX * th * aspect + u[i] * ndcY * th);
    for (int i = 0; i < 3; ++i) target_[i] = p[i] + (target_[i] - p[i]) * factor;
    dist_ = std::max(dist_ * factor, radius_ * 1e-4f);
}

Mat4 Camera::view() const {
    float r[3], u[3], b[3], e[3];
    basis(r, u, b);
    eye(e);
    Mat4 v;
    float* m = v.m;
    m[0] = r[0], m[4] = r[1], m[8] = r[2], m[12] = -dot3(r, e);
    m[1] = u[0], m[5] = u[1], m[9] = u[2], m[13] = -dot3(u, e);
    m[2] = b[0], m[6] = b[1], m[10] = b[2], m[14] = -dot3(b, e);
    m[3] = 0, m[7] = 0, m[11] = 0, m[15] = 1;
    return v;
}

Mat4 Camera::projection(float aspect) const {
    // Depth range from the distance to the model centre so that zooming
    // towards an off-centre point keeps the whole model inside [near, far].
    float e[3], c[3];
    eye(e);
    for (int i = 0; i < 3; ++i) c[i] = 0.5f * (bounds_.min[i] + bounds_.max[i]) - e[i];
    float dc = std::sqrt(dot3(c, c));
    float zn = std::max({dc - 3.0f * radius_, dist_ * 0.01f, 1e-4f});
    float zf = dc + 3.0f * radius_;
    float f = 1.0f / std::tan(kFovY * 0.5f);
    Mat4 p;
    float* m = p.m;
    m[0] = f / aspect, m[5] = f;
    m[10] = (zf + zn) / (zn - zf), m[11] = -1;
    m[14] = 2 * zf * zn / (zn - zf), m[15] = 0;
    return p;
}

}  // namespace v3d
