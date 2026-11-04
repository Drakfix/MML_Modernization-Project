#version 450
// the atlas holds coverage in red; texel (0,0) is solid for plain shapes (game.hlsl's ps_ui)
layout(set = 0, binding = 0) uniform sampler smp;
layout(set = 1, binding = 1) uniform texture2D font;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main()
{
    o_color = vec4(v_color.rgb, v_color.a * texture(sampler2D(font, smp), v_uv).r);
}
