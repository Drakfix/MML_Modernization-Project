// Shaders for the game's screen-space ("transformed and lit") triangles.
// Built with fxc into headers by native\build.bat (shader model 4.0).

cbuffer Draw : register(b0)
{
    float2 app_size;      // the game's display mode, e.g. 854 x 480
    float textured;
    float flat;
    float4 fog_color;     // rgb
    float fog_start;
    float fog_end;
    float fog_on;
    float fog_from_w;     // 1 = fog depth is 1/rhw, 0 = z
    float alpha_test;
    float alpha_func;     // D3DCMPFUNC
    float alpha_ref;      // 0..1
    float pad;
};

Texture2D tex : register(t0);
SamplerState smp : register(s0);

struct VSIn
{
    float4 pos : POSITION;    // x, y in pixels, z in 0..1, rhw
    float4 diffuse : COLOR0;
    float4 specular : COLOR1;
    float2 uv : TEXCOORD0;
};

struct PSIn
{
    float4 pos : SV_Position;
    float4 color : COLOR0;
    nointerpolation float4 flat_color : COLOR1;
    float2 uv : TEXCOORD0;
    float fog : FOG;
};

PSIn vs_main(VSIn i)
{
    PSIn o;
    float w = 1.0 / max(i.pos.w, 1e-10);
    // Direct3D 9-era vertices put pixel centres on whole numbers; D3D10+ puts
    // them on .5, so shift by half a pixel before going to clip space.
    float2 ndc = float2((i.pos.x + 0.5) / app_size.x * 2.0 - 1.0,
                        1.0 - (i.pos.y + 0.5) / app_size.y * 2.0);
    // multiply through by w so texture coordinates and colours interpolate
    // with perspective, exactly like rhw did on the old pipeline
    o.pos = float4(ndc * w, i.pos.z * w, w);
    o.color = i.diffuse;
    o.flat_color = i.diffuse;
    o.uv = i.uv;
    float depth = fog_from_w > 0.5 ? w : i.pos.z;
    o.fog = fog_on > 0.5 ? saturate((fog_end - depth) / max(fog_end - fog_start, 1e-6)) : 1.0;
    return o;
}

bool alpha_pass(float a)
{
    int f = (int)alpha_func;
    if (f == 1) return false;                  // NEVER
    if (f == 2) return a < alpha_ref;          // LESS
    if (f == 3) return a == alpha_ref;         // EQUAL
    if (f == 4) return a <= alpha_ref;         // LESSEQUAL
    if (f == 5) return a > alpha_ref;          // GREATER
    if (f == 6) return a != alpha_ref;         // NOTEQUAL
    if (f == 7) return a >= alpha_ref;         // GREATEREQUAL
    return true;                               // ALWAYS
}

struct PSOut
{
    float4 color : SV_Target0;
    float4 depth : SV_Target1;  // effects depth in .r, written only for world geometry (blend state);
                                // .a = coverage, so alpha-blended pixels count as much as they cover
};

PSOut ps_main(PSIn i)
{
    PSOut o;
    float4 c = flat > 0.5 ? i.flat_color : i.color;
    if (textured > 0.5)
        c *= tex.Sample(smp, i.uv);
    if (alpha_test > 0.5 && !alpha_pass(c.a))
        discard;
    c.rgb = lerp(fog_color.rgb, c.rgb, i.fog);
    o.color = c;
    // The game's depth is linear (camera z / 16384). Effects tools expect a
    // perspective depth buffer: store it the way ReShade's default
    // linearisation (far plane 1000) reads back as the same linear value.
    o.depth = float4(i.pos.z * 1000.0 / (1.0 + i.pos.z * 999.0), 0, 0, c.a);
    return o;
}

// ---- final blit of the rendered frame to the window ----

Texture2D scene : register(t0);
SamplerState scene_smp : register(s0);

struct BlitOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

BlitOut vs_blit(uint id : SV_VertexID)
{
    BlitOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 ps_blit(BlitOut i) : SV_Target
{
    return scene.Sample(scene_smp, i.uv);
}

// end of frame: the effects depth (every visible surface, including the ones
// the game draws without writing depth) replaces the depth buffer
Texture2D<float> effects_depth : register(t0);

float ps_depth_copy(BlitOut i) : SV_Depth
{
    return effects_depth.Load(int3(i.pos.xy, 0));
}

// [debug] show_depth=1: the effects depth instead of the picture (linear, x4)
float4 ps_show_depth(BlitOut i) : SV_Target
{
    uint w, h;
    effects_depth.GetDimensions(w, h);
    float d = effects_depth.Load(int3(i.uv * float2(w, h), 0));
    d = d / (1000.0 - d * 999.0);
    return float4(saturate(d * 4.0).xxx, 1.0);
}
