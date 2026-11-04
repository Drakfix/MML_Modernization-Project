#version 450
// our UI in window pixels (game.hlsl's vs_ui)
layout(push_constant) uniform UI
{
    vec2 inv_size;   // 1 / window size
} pc;

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;   // 0xAARRGGBB read as R8G8B8A8

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main()
{
    gl_Position = vec4(in_pos * pc.inv_size * 2.0 - 1.0, 0.0, 1.0);
    v_uv = in_uv;
    v_color = in_color.bgra;
}
