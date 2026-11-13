#version 450
// The game's screen-space ("transformed and lit") triangles: game.hlsl's vs_main for Vulkan
#include "draw.glsl"

layout(location = 0) in vec4 in_pos;        // x, y in pixels, z in 0..1, rhw
layout(location = 1) in vec4 in_diffuse;    // 0xAARRGGBB read as R8G8B8A8: .bgra is r, g, b, a
layout(location = 3) in vec2 in_uv;

layout(location = 0) out vec4 v_color;
layout(location = 1) flat out vec4 v_flat;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out float v_fog;

// camera z / 16384 goes past 1 beyond 16384 units: kept up to the knee, squeezed below 1
// after it, so the far side of the big maps isn't clipped (game.hlsl's depth_range)
const float DEPTH_KNEE = 0.9;
float depth_range(float z)
{
    float s = max(z - DEPTH_KNEE, 0.0) / (1.0 - DEPTH_KNEE);
    return z <= DEPTH_KNEE ? z : DEPTH_KNEE + (1.0 - DEPTH_KNEE) * s / (1.0 + s);
}

void main()
{
    float w = 1.0 / max(in_pos.w, 1e-10);
    // the game's screen edges at 0 and its mode's size: whole numbers are pixel corners, as
    // the PlayStation drew them (Direct3D 9's half-pixel offset is exact only unscaled; scaled
    // up, a full-screen fade left the first row and column uncovered); Vulkan's framebuffer y
    // points down, as the game's does
    vec2 ndc = vec2(in_pos.x / pc.app_size.x * 2.0 - 1.0, in_pos.y / pc.app_size.y * 2.0 - 1.0);
    // multiplied through by w so texture coordinates and colours interpolate with perspective
    gl_Position = vec4(ndc * w, depth_range(in_pos.z) * w, w);
    v_color = in_diffuse.bgra;
    v_flat = in_diffuse.bgra;
    v_uv = in_uv;
    float depth = pc.fog_from_w > 0.5 ? w : in_pos.z;
    v_fog = pc.fog_on > 0.5 ? clamp((pc.fog_end - depth) / max(pc.fog_end - pc.fog_start, 1e-6), 0.0, 1.0) : 1.0;
}
