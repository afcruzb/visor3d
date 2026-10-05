#pragma once

// Lighting model shared by the viewer's fragment shader (renderer.cpp, which
// receives these constants through glslDefines()) and the CPU thumbnail
// rasterizer (Light below). Edit the constants here and both stay identical.
//
// Model: light from the viewer, offset towards the upper left; sky/ground
// hemisphere ambient; small Blinn specular; lit in linear space.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace v3d::shading {

inline constexpr float kGround[3] = {0.07f, 0.065f, 0.06f};
inline constexpr float kSky[3] = {0.30f, 0.31f, 0.34f};
inline constexpr float kLightOffset[3] = {-0.35f, 0.55f, 0.0f};  // added to the view direction
inline constexpr float kDiffuse = 0.85f;
inline constexpr float kSpecPower = 40.0f;
inline constexpr float kSpecStrength = 0.10f;
inline constexpr float kGamma = 2.2f;

inline float toLinear(float srgb) { return std::pow(srgb, kGamma); }
inline float toDisplay(float linear) { return std::pow(std::clamp(linear, 0.f, 1.f), 1.f / kGamma); }

// The constants above as GLSL #defines, for the viewer's fragment shader.
inline std::string glslDefines() {
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "#define GROUND vec3(%.4f, %.4f, %.4f)\n"
                  "#define SKY vec3(%.4f, %.4f, %.4f)\n"
                  "#define LIGHT_OFFSET vec3(%.4f, %.4f, %.4f)\n"
                  "#define DIFFUSE %.4f\n"
                  "#define SPEC_POWER %.4f\n"
                  "#define SPEC_STRENGTH %.4f\n"
                  "#define GAMMA %.4f\n",
                  kGround[0], kGround[1], kGround[2], kSky[0], kSky[1], kSky[2], kLightOffset[0], kLightOffset[1],
                  kLightOffset[2], kDiffuse, kSpecPower, kSpecStrength, kGamma);
    return buf;
}

// CPU twin of the fragment shader for a fixed view direction (orthographic
// views). All vectors are unit length, in view space.
class Light {
public:
    Light(const float toViewer[3], const float worldUp[3]) {
        float l[3], h[3];
        for (int i = 0; i < 3; ++i) l[i] = toViewer[i] + kLightOffset[i];
        normalize(l);
        for (int i = 0; i < 3; ++i) h[i] = l[i] + toViewer[i];
        normalize(h);
        for (int i = 0; i < 3; ++i) v_[i] = toViewer[i], l_[i] = l[i], h_[i] = h[i], up_[i] = worldUp[i];
    }

    // base and out are linear RGB.
    void shade(const float normal[3], const float base[3], float out[3]) const {
        float n[3] = {normal[0], normal[1], normal[2]};
        if (dot(n, v_) < 0) n[0] = -n[0], n[1] = -n[1], n[2] = -n[2];  // two-sided
        float diff = std::max(dot(n, l_), 0.f);
        float hemi = dot(n, up_) * 0.5f + 0.5f;
        float spec = std::pow(std::max(dot(n, h_), 0.f), kSpecPower) * kSpecStrength;
        for (int i = 0; i < 3; ++i) {
            float amb = kGround[i] + (kSky[i] - kGround[i]) * hemi;
            out[i] = base[i] * (amb + kDiffuse * diff) + spec;
        }
    }

private:
    static float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    static void normalize(float a[3]) {
        float len = std::sqrt(dot(a, a));
        for (int i = 0; i < 3; ++i) a[i] /= len;
    }

    float v_[3], l_[3], h_[3], up_[3];
};

}  // namespace v3d::shading
