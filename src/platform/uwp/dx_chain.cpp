/*
 * SwanStationUWP - the multi-pass shader chains on Direct3D 11. See dx_chain.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ported from the Vulkan renderer's chain runner: the same pass tables, the same meaning of the push constants (register b1)
 * and of the parameter block (register b0), the textures at the registers of their bindings.
 */
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <stdio.h>
#include <string.h>

#include <vector>

extern "C"
{
#include "../platform.h"
}
#include "dx_chain.h"
#include "shaders_crt.h"
#include "shaders_ntsc.h"
#include "shaders_royale.h"

using Microsoft::WRL::ComPtr;

#define MAXP 12 /* passes of a shader chain */

struct DxChain
{
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    bool ok = false;
    bool offscreen = false;
    int preset = -1;
    const RoyalePass *passes = nullptr;
    const RoyaleTexture *textures = nullptr;
    int n = 0, tex_n = 0;
    bool feed[MAXP] = {}; // a pass whose previous frame a pass reads: two images, taken in turns

    ComPtr<ID3D11VertexShader> vs[MAXP];
    ComPtr<ID3D11PixelShader> ps[MAXP];
    ComPtr<ID3D11Buffer> ubo;            // the shared parameter block
    ComPtr<ID3D11Buffer> pass_ubo[MAXP]; // a pass's own (shaders_crt.h)
    std::vector<uint8_t> pass_ubo_data[MAXP];
    ComPtr<ID3D11Buffer> push;
    ComPtr<ID3D11SamplerState> sampler[2][3]; // [linear][border, edge, repeat]
    ComPtr<ID3D11ShaderResourceView> tex_view[8];
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;

    struct Out
    {
        ComPtr<ID3D11Texture2D> texture[2];
        ComPtr<ID3D11ShaderResourceView> view[2];
        ComPtr<ID3D11RenderTargetView> target[2];
    } out[MAXP]; // the images of every pass but the last
    int size[MAXP][2] = {};
    int native[2] = {};
    int source_size[2] = {};
    int built[4] = {};
    bool have_images = false;
    uint32_t frames = 0, frame_now = 0;
};

namespace
{
int royale_cur(const DxChain *c, int i)
{
    return c->feed[i] ? (int)(c->frame_now & 1) : 0;
}

int royale_scale(int type, float scale, int source, int viewport)
{
    float v = type == ROYALE_SCALE_SOURCE ? source * scale : type == ROYALE_SCALE_VIEWPORT ? viewport * scale : scale;
    int n = (int)(v + 0.5f);
    return n < 1 ? 1 : n;
}

void size_vec(float *v, int w, int h)
{
    v[0] = (float)w;
    v[1] = (float)h;
    v[2] = 1.0f / (float)w;
    v[3] = 1.0f / (float)h;
}

// One member of a pass's push constants or uniform block.
void member(const DxChain *c, int i, const RoyalePush *m, uint8_t *data)
{
    const RoyalePass *p = &c->passes[i];
    int in_w = i == 0 ? c->native[0] : c->size[i - 1][0], in_h = i == 0 ? c->native[1] : c->size[i - 1][1];
    float *v = (float *)(data + m->offset);
    switch (m->kind)
    {
    case ROYALE_PUSH_SOURCE_SIZE: size_vec(v, in_w, in_h); break;
    case ROYALE_PUSH_ORIGINAL_SIZE: size_vec(v, c->native[0], c->native[1]); break;
    case ROYALE_PUSH_OUTPUT_SIZE: size_vec(v, c->size[i][0], c->size[i][1]); break;
    case ROYALE_PUSH_FINAL_SIZE: size_vec(v, c->size[c->n - 1][0], c->size[c->n - 1][1]); break;
    case ROYALE_PUSH_FRAME:
    {
        uint32_t frame = p->frame_mod ? c->frame_now % p->frame_mod : c->frame_now;
        memcpy(v, &frame, sizeof(uint32_t));
        break;
    }
    case ROYALE_PUSH_PASS_SIZE: size_vec(v, c->size[m->index][0], c->size[m->index][1]); break;
    case ROYALE_PUSH_TEXTURE_SIZE: size_vec(v, c->textures[m->index].width, c->textures[m->index].height); break;
    case ROYALE_PUSH_PARAM: *v = m->value; break;
    default: break;
    }
}

ComPtr<ID3D11Buffer> make_constants(ID3D11Device *device, const void *data, UINT bytes)
{
    ComPtr<ID3D11Buffer> buffer;
    const UINT size = (bytes + 15) / 16 * 16;
    std::vector<uint8_t> padded(size, 0);
    if (data)
        memcpy(padded.data(), data, bytes);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = size;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init = {padded.data(), 0, 0};
    device->CreateBuffer(&bd, &init, buffer.GetAddressOf());
    return buffer;
}

void free_images(DxChain *c)
{
    for (int i = 0; i < MAXP; ++i)
        for (int b = 0; b < 2; ++b)
        {
            c->out[i].target[b].Reset();
            c->out[i].view[b].Reset();
            c->out[i].texture[b].Reset();
        }
    c->have_images = false;
}

void free_all(DxChain *c)
{
    free_images(c);
    for (int i = 0; i < MAXP; ++i)
    {
        c->vs[i].Reset();
        c->ps[i].Reset();
        c->pass_ubo[i].Reset();
        c->pass_ubo_data[i].clear();
        c->feed[i] = false;
    }
    for (int t = 0; t < 8; ++t)
        c->tex_view[t].Reset();
    c->ubo.Reset();
    c->push.Reset();
    for (int l = 0; l < 2; ++l)
        for (int w = 0; w < 3; ++w)
            c->sampler[l][w].Reset();
    c->ok = false;
    c->passes = nullptr;
    c->textures = nullptr;
    c->n = c->tex_n = 0;
    c->preset = -1;
}

bool init(DxChain *c)
{
    ID3D11Device *d = c->device;
    static const D3D11_TEXTURE_ADDRESS_MODE wraps[3] = {D3D11_TEXTURE_ADDRESS_BORDER, D3D11_TEXTURE_ADDRESS_CLAMP,
                                                        D3D11_TEXTURE_ADDRESS_WRAP};
    for (int linear = 0; linear < 2; ++linear)
        for (int w = 0; w < 3; ++w)
        {
            D3D11_SAMPLER_DESC sd = {};
            sd.Filter = linear ? D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = wraps[w];
            sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            if (FAILED(d->CreateSamplerState(&sd, c->sampler[linear][w].GetAddressOf())))
                return false;
        }
    D3D11_RASTERIZER_DESC rs = {};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.ScissorEnable = TRUE;
    rs.DepthClipEnable = TRUE;
    if (FAILED(d->CreateRasterizerState(&rs, c->rasterizer.GetAddressOf())))
        return false;
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(d->CreateBlendState(&bd, c->blend.GetAddressOf())))
        return false;
    D3D11_DEPTH_STENCIL_DESC dsd = {};
    if (FAILED(d->CreateDepthStencilState(&dsd, c->depth.GetAddressOf())))
        return false;

    // the parameters: set once, from the defaults
    c->ubo = make_constants(d, ROYALE_UBO, sizeof(ROYALE_UBO));
    c->push = make_constants(d, nullptr, 128);
    if (!c->ubo || !c->push)
        return false;
    // the passes with a uniform block of their own: the renderer writes sizes into it each frame
    for (int i = 0; i < c->n; ++i)
    {
        const RoyalePass *p = &c->passes[i];
        if (!p->ubo_blob)
            continue;
        c->pass_ubo_data[i].assign(p->ubo_blob, p->ubo_blob + p->ubo_size);
        c->pass_ubo_data[i].resize((p->ubo_size + 15) / 16 * 16, 0);
        c->pass_ubo[i] = make_constants(d, p->ubo_blob, p->ubo_size);
        if (!c->pass_ubo[i])
            return false;
    }
    for (int t = 0; t < c->tex_n; ++t)
    {
        const RoyaleTexture *tex = &c->textures[t];
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = tex->width;
        td.Height = tex->height;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = {tex->rgba, (UINT)tex->width * 4, 0};
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(d->CreateTexture2D(&td, &init, texture.GetAddressOf())) ||
            FAILED(d->CreateShaderResourceView(texture.Get(), nullptr, c->tex_view[t].GetAddressOf())))
            return false;
    }
    for (int i = 0; i < c->n; ++i)
    {
        const RoyalePass *p = &c->passes[i];
        if (FAILED(d->CreateVertexShader(p->vert, p->vert_size, nullptr, c->vs[i].GetAddressOf())) ||
            FAILED(d->CreatePixelShader(p->frag, p->frag_size, nullptr, c->ps[i].GetAddressOf())))
            return false;
    }
    return true;
}

// Draws pass i into the target and viewport the caller set.
void draw_pass(DxChain *c, int i, ID3D11ShaderResourceView *source, bool source_scaled)
{
    ID3D11DeviceContext *cx = c->context;
    const RoyalePass *p = &c->passes[i];

    uint8_t push[128];
    memset(push, 0, sizeof(push));
    for (int k = 0; k < p->push_count; ++k)
        member(c, i, &p->push[k], push);
    cx->UpdateSubresource(c->push.Get(), 0, nullptr, push, 0, 0);

    ID3D11Buffer *params = c->ubo.Get();
    if (p->ubo_blob)
    {
        for (int k = 0; k < p->ubo_sem_count; ++k)
            member(c, i, &p->ubo_sem[k], c->pass_ubo_data[i].data());
        cx->UpdateSubresource(c->pass_ubo[i].Get(), 0, nullptr, c->pass_ubo_data[i].data(), 0, 0);
        params = c->pass_ubo[i].Get();
    }

    ID3D11ShaderResourceView *views[10] = {};
    ID3D11SamplerState *samplers[10] = {};
    for (int s = 0; s < p->sampler_count && s < 8; ++s)
    {
        const RoyaleSampler *sm = &p->samplers[s];
        ID3D11ShaderResourceView *view = nullptr;
        ID3D11SamplerState *sampler = c->sampler[p->linear][p->wrap].Get();
        switch (sm->kind)
        {
        case ROYALE_SAMPLER_SOURCE:
            if (i == 0)
            {
                view = source;
                // the PS1's picture is made from the bigger one, smoothly; a first pass that is not smaller than the
                // picture reads it as the preset says
                const bool smaller = source_scaled || c->size[0][0] < c->source_size[0] || c->size[0][1] < c->source_size[1];
                if (smaller)
                    sampler = c->sampler[1][1].Get();
            }
            else
                view = c->out[i - 1].view[royale_cur(c, i - 1)].Get();
            break;
        case ROYALE_SAMPLER_ORIGINAL:
            view = source;
            sampler = c->sampler[1][1].Get();
            break;
        case ROYALE_SAMPLER_PASS: view = c->out[sm->index].view[royale_cur(c, sm->index)].Get(); break;
        case ROYALE_SAMPLER_FEEDBACK: view = c->out[sm->index].view[royale_cur(c, sm->index) ^ 1].Get(); break;
        default:
            view = c->tex_view[sm->index].Get();
            sampler = c->sampler[c->textures[sm->index].linear][c->textures[sm->index].wrap].Get();
            break;
        }
        if (sm->binding < 10)
        {
            views[sm->binding] = view;
            samplers[sm->binding] = sampler;
        }
    }

    cx->IASetInputLayout(nullptr);
    cx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    cx->VSSetShader(c->vs[i].Get(), nullptr, 0);
    cx->PSSetShader(c->ps[i].Get(), nullptr, 0);
    ID3D11Buffer *buffers[2] = {params, c->push.Get()};
    cx->VSSetConstantBuffers(0, 2, buffers);
    cx->PSSetConstantBuffers(0, 2, buffers);
    cx->PSSetShaderResources(0, 10, views);
    cx->PSSetSamplers(0, 10, samplers);
    cx->RSSetState(c->rasterizer.Get());
    cx->OMSetBlendState(c->blend.Get(), nullptr, 0xffffffffu);
    cx->OMSetDepthStencilState(c->depth.Get(), 0);
    cx->Draw(4, 0);
    ID3D11ShaderResourceView *none[10] = {};
    cx->PSSetShaderResources(0, 10, none);
}
} // namespace

DxChain *dxc_create(ID3D11Device *device, ID3D11DeviceContext *context)
{
    DxChain *c = new DxChain();
    c->device = device;
    c->context = context;
    return c;
}

void dxc_destroy(DxChain *c)
{
    if (!c)
        return;
    free_all(c);
    delete c;
}

bool dxc_prepare(DxChain *c, int preset, bool offscreen, int native_w, int native_h, int viewport_w, int viewport_h,
                 int source_w, int source_h)
{
    if (c->preset != preset)
    {
        // another chain than the one made: everything of the old one goes
        free_all(c);
        c->preset = preset;
        if (preset >= 6)
        {
            c->passes = CRT_PRESETS[preset - 6].passes;
            c->n = CRT_PRESETS[preset - 6].count;
            c->textures = CRT_PRESETS[preset - 6].textures;
            c->tex_n = CRT_PRESETS[preset - 6].tex_count;
        }
        else
        {
            c->passes = preset ? NTSC_PRESETS[preset - 1].passes : ROYALE_PASSES;
            c->n = preset ? NTSC_PRESETS[preset - 1].count : (int)(sizeof(ROYALE_PASSES) / sizeof(ROYALE_PASSES[0]));
            c->textures = preset ? nullptr : ROYALE_TEXTURES;
            c->tex_n = preset ? 0 : (int)(sizeof(ROYALE_TEXTURES) / sizeof(ROYALE_TEXTURES[0]));
        }
        for (int i = 0; i < c->n; ++i)
            for (int k = 0; k < c->passes[i].sampler_count; ++k)
                if (c->passes[i].samplers[k].kind == ROYALE_SAMPLER_FEEDBACK)
                    c->feed[c->passes[i].samplers[k].index] = true;
        c->offscreen = offscreen;
        c->ok = init(c);
        if (!c->ok)
        {
            SwanStationPS5_log("d3d11: shader chain %d could not be set up", preset);
            const int keep = c->preset;
            free_all(c);
            c->preset = keep; // not tried again until another chain is asked for
        }
    }
    if (!c->ok)
        return false;
    c->native[0] = native_w;
    c->native[1] = native_h;
    c->source_size[0] = source_w;
    c->source_size[1] = source_h;
    int in_w = native_w, in_h = native_h;
    for (int i = 0; i < c->n; ++i)
    {
        const RoyalePass *p = &c->passes[i];
        int w = i == c->n - 1 ? viewport_w : royale_scale(p->scale_x_type, p->scale_x, in_w, viewport_w);
        int h = i == c->n - 1 ? viewport_h : royale_scale(p->scale_y_type, p->scale_y, in_h, viewport_h);
        c->size[i][0] = in_w = w;
        c->size[i][1] = in_h = h;
    }
    const int key[4] = {native_w, native_h, viewport_w, viewport_h};
    if (c->have_images && memcmp(key, c->built, sizeof(key)) == 0)
        return true;
    free_images(c);
    static const float black[4] = {0, 0, 0, 0};
    for (int i = 0; i < (offscreen ? c->n : c->n - 1); ++i)
    {
        const RoyalePass *p = &c->passes[i];
        const DXGI_FORMAT format = p->srgb == 2 ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                   : p->srgb    ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                : DXGI_FORMAT_R8G8B8A8_UNORM;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = (UINT)c->size[i][0];
        td.Height = (UINT)c->size[i][1];
        td.MipLevels = td.ArraySize = 1;
        td.Format = format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (int b = 0; b < (c->feed[i] ? 2 : 1); ++b)
        {
            DxChain::Out &o = c->out[i];
            if (FAILED(c->device->CreateTexture2D(&td, nullptr, o.texture[b].GetAddressOf())) ||
                FAILED(c->device->CreateShaderResourceView(o.texture[b].Get(), nullptr, o.view[b].GetAddressOf())) ||
                FAILED(c->device->CreateRenderTargetView(o.texture[b].Get(), nullptr, o.target[b].GetAddressOf())))
            {
                SwanStationPS5_log("d3d11: shader chain %d: no memory for its images", preset);
                free_images(c);
                c->ok = false;
                return false;
            }
            c->context->ClearRenderTargetView(o.target[b].Get(), black); // what a pass read before it was ever drawn
        }
    }
    memcpy(c->built, key, sizeof(key));
    c->have_images = true;
    SwanStationPS5_log("d3d11: shader chain %d: PS1 %dx%d, viewport %dx%d", preset, native_w, native_h, viewport_w,
                       viewport_h);
    return true;
}

void dxc_record(DxChain *c, ID3D11ShaderResourceView *source, bool source_scaled)
{
    c->frame_now = c->frames;
    for (int i = 0; i < (c->offscreen ? c->n : c->n - 1); ++i)
    {
        const int cur = royale_cur(c, i);
        c->context->OMSetRenderTargets(1, c->out[i].target[cur].GetAddressOf(), nullptr);
        D3D11_VIEWPORT vp = {0, 0, (float)c->size[i][0], (float)c->size[i][1], 0, 1};
        c->context->RSSetViewports(1, &vp);
        D3D11_RECT scissor = {0, 0, c->size[i][0], c->size[i][1]};
        c->context->RSSetScissorRects(1, &scissor);
        draw_pass(c, i, source, source_scaled);
    }
    c->context->OMSetRenderTargets(0, nullptr, nullptr);
    ++c->frames;
}

void dxc_final(DxChain *c, ID3D11ShaderResourceView *source, bool source_scaled, int x, int y, int w, int h, int target_w,
               int target_h)
{
    D3D11_VIEWPORT vp = {(float)x, (float)y, (float)w, (float)h, 0, 1};
    c->context->RSSetViewports(1, &vp);
    D3D11_RECT scissor = {x < 0 ? 0 : x, y < 0 ? 0 : y, x + w > target_w ? target_w : x + w, y + h > target_h ? target_h : y + h};
    if (scissor.right <= scissor.left || scissor.bottom <= scissor.top)
        return;
    c->context->RSSetScissorRects(1, &scissor);
    draw_pass(c, c->n - 1, source, source_scaled);
}

ID3D11ShaderResourceView *dxc_output(DxChain *c)
{
    return c->n > 0 ? c->out[c->n - 1].view[royale_cur(c, c->n - 1)].Get() : nullptr;
}
