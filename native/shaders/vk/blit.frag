#version 450
// the scene image into the window (game.hlsl's ps_blit)
layout(set = 0, binding = 0) uniform sampler smp;
layout(set = 1, binding = 1) uniform texture2D scene;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main()
{
    o_color = texture(sampler2D(scene, smp), v_uv);
}
