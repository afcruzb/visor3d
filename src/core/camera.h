#pragma once

#include "mesh.h"

namespace v3d {

enum class ViewPreset { Front, Back, Left, Right, Top, Bottom, Iso };

// Isometric preset, also the default orientation (viewer and thumbnails):
// looking from the front-right, 30 degrees above the bed.
constexpr float kIsoYaw = -1.0472f;
constexpr float kIsoPitch = 0.5236f;

// Z-up orbit camera (3D printing convention).
class Camera {
public:
    void fit(const BBox& b, float aspect);
    void refit(float aspect) { fit(bounds_, aspect); }
    void setPreset(ViewPreset p, float aspect);
    void orbit(float dxPixels, float dyPixels);
    void pan(float dxPixels, float dyPixels, float viewportHeight);
    void zoom(float factor, float ndcX, float ndcY, float aspect);

    Mat4 view() const;
    Mat4 projection(float aspect) const;
    // Camera axes in world space: right, up, back (towards the viewer).
    void basis(float right[3], float up[3], float back[3]) const;
    void eye(float out[3]) const;

private:
    static constexpr float kFovY = 0.5236f;  // 30 degrees

    BBox bounds_;
    float target_[3] = {0, 0, 0};
    float yaw_ = kIsoYaw;
    float pitch_ = kIsoPitch;
    float dist_ = 100;
    float radius_ = 50;
};

}  // namespace v3d
