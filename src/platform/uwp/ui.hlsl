// SwanStationUWP - the interface drawn by the GPU: the texture times the vertex colour.
// SPDX-License-Identifier: GPL-3.0-or-later
// Compiled into shaders_d3d.h by tools/make-d3d-shaders.ps1.

cbuffer Canvas : register(b0)
{
    float2 canvas; // the interface's coordinate space (1920x1080)
};

Texture2D picture : register(t0);
SamplerState linear_sampler : register(s0);

struct VsIn
{
    float2 pos : POSITION;
    float2 uv : TEXCOORD0;
    float4 colour : COLOR0;
};

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 colour : COLOR0;
};

VsOut vs_main(VsIn i)
{
    VsOut o;
    o.pos = float4(i.pos.x / canvas.x * 2.0 - 1.0, 1.0 - i.pos.y / canvas.y * 2.0, 0.0, 1.0);
    o.uv = i.uv;
    o.colour = i.colour;
    return o;
}

float4 ps_main(VsOut i) : SV_Target
{
    return picture.Sample(linear_sampler, i.uv) * i.colour;
}
