// The game's draw constants (gfx_vk.c's push constants; game.hlsl's cbuffer Draw)
layout(push_constant) uniform Draw
{
    vec2 app_size;      // the game's display mode, e.g. 854 x 480
    float textured;
    float flat_;
    vec4 fog_color;     // rgb
    float fog_start;
    float fog_end;
    float fog_on;
    float fog_from_w;   // 1 = fog depth is 1/rhw, 0 = z
    float alpha_test;
    float alpha_func;   // D3DCMPFUNC
    float alpha_ref;    // 0..1
    float pad;
} pc;
