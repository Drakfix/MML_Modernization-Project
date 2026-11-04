#version 450
// game.hlsl's ps_main for Vulkan (no effects depth target yet)
#include "draw.glsl"

layout(set = 0, binding = 0) uniform sampler smp;
layout(set = 1, binding = 1) uniform texture2D tex;

layout(location = 0) in vec4 v_color;
layout(location = 1) flat in vec4 v_flat;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in float v_fog;

layout(location = 0) out vec4 o_color;

bool alpha_pass(float a)
{
    int f = int(pc.alpha_func);
    if (f == 1) return false;                  // NEVER
    if (f == 2) return a < pc.alpha_ref;       // LESS
    if (f == 3) return a == pc.alpha_ref;      // EQUAL
    if (f == 4) return a <= pc.alpha_ref;      // LESSEQUAL
    if (f == 5) return a > pc.alpha_ref;       // GREATER
    if (f == 6) return a != pc.alpha_ref;      // NOTEQUAL
    if (f == 7) return a >= pc.alpha_ref;      // GREATEREQUAL
    return true;                               // ALWAYS
}

void main()
{
    vec4 c = pc.flat_ > 0.5 ? v_flat : v_color;
    if (pc.textured > 0.5)
        c *= texture(sampler2D(tex, smp), v_uv);
    if (pc.alpha_test > 0.5 && !alpha_pass(c.a))
        discard;
    c.rgb = mix(pc.fog_color.rgb, c.rgb, v_fog);
    o_color = c;
}
