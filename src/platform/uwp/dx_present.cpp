/*
 * SwanStationUWP - the screen through Direct3D 11. See dx_present.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The frame is a list: the interface's triangles (dxp_ui_draw) and, in its place, the game's picture (dxp_ui_game_here).
 * dxp_present_ui() uploads the vertices once, plays the list in order on the swap chain's buffer and presents with a
 * sync interval of 1 (the wait for the TV's refresh).
 */
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <stdio.h>
#include <string.h>

#include <vector>

extern "C"
{
#include "libretro.h"
#include "../platform.h"
}
#include "libretro_d3d11.h"

#include "dx_chain.h"
#include "dx_present.h"
#include "shaders_d3d.h"
#include "shaders_game.h"
#include "uwp_app.h"


using Microsoft::WRL::ComPtr;

namespace
{
struct Texture
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    int width = 0, height = 0;
    bool smooth = true, mips = false, used = false;
};

struct Command
{
    bool game;     // the game's picture instead of triangles
    int texture;   // -1: the white pixel
    bool blend;
    bool clipped;
    int clip[4];   // canvas pixels
    UINT first, count;
};

struct Screen
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapchain;
    ComPtr<ID3D11RenderTargetView> target;
    int canvas_w = 1920, canvas_h = 1080;
    int width = 0, height = 0; // the swap chain's buffers
    int want_ext_w = 0, want_ext_h = 0;
    double hz = 0, mode_hz = 0, measured_hz = 0;
    uint64_t last_present_us = 0, interval_sum_us = 0;
    int interval_count = 0;
    char description[96] = "";

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> constants, vertices;
    UINT vertices_capacity = 0;
    ComPtr<ID3D11BlendState> blend_on, blend_off;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11SamplerState> linear_mip, linear_flat, point;
    int white = -1;
    bool ui_ready = false;

    std::vector<Texture> textures;
    std::vector<DxpUiVertex> list;
    std::vector<Command> commands;
    bool blend = true, clipped = false;
    int clip[4] = {};
    float clear[4] = {};

    // the core's picture
    ComPtr<ID3D11ShaderResourceView> game_view;
    unsigned game_w = 0, game_h = 0;
    bool game_nearest = false;
    float game_rect[4] = {}, game_crop = 0;
    bool game_shown = false;
    int game_shader = 0, game_tex_w = 0, game_tex_h = 0, game_lines = 240;
    float colour[4] = {1.0f, 1.0f, 0.0f, 0.0f}; // brightness, saturation, warmth, sharpening
    bool fsr = false;

    // the game's own shaders: the picture through one pixel shader (game_ps), or through a chain of passes
    ComPtr<ID3D11VertexShader> game_vs;
    ComPtr<ID3D11PixelShader> game_ps[6]; // the plain picture, LCD3x, supersampling, then shaders 9, 10 and 12
    ComPtr<ID3D11Buffer> game_push;
    DxChain *chain = nullptr;     // CRT Royale, NTSC and the other presets
    DxChain *fsr_chain = nullptr; // FSR 1, before the picture's shader

    retro_hw_render_interface_d3d11 core = {};
};
Screen S;

#define CHECK(call, what)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        HRESULT hr_ = (call);                                                                                          \
        if (FAILED(hr_))                                                                                               \
        {                                                                                                              \
            snprintf(error, size, "%s failed (0x%08lx)", what, (unsigned long)hr_);                                    \
            return false;                                                                                              \
        }                                                                                                              \
    } while (0)

void describe()
{
    int w = 0, h = 0;
    double hz = 0;
    // the TV's mode when the system tells it; else (a PC) 0 until the present's own rate has been measured: guessing 60 Hz
    // on a faster monitor would make the pacing run the game at that monitor's speed
    S.mode_hz = uwp_current_display_mode(&w, &h, &hz) && hz > 0 ? hz : 0.0;
    S.hz = S.measured_hz > 0 ? S.measured_hz : S.mode_hz;
    snprintf(S.description, sizeof(S.description), "%dx%d @ %.2f Hz", S.width, S.height, S.hz > 0 ? S.hz : 0.0);
}

bool make_target(char *error, size_t size)
{
    ComPtr<ID3D11Texture2D> buffer;
    CHECK(S.swapchain->GetBuffer(0, IID_PPV_ARGS(buffer.GetAddressOf())), "swap chain buffer");
    CHECK(S.device->CreateRenderTargetView(buffer.Get(), nullptr, S.target.ReleaseAndGetAddressOf()), "render target");
    return true;
}

bool make_swapchain(char *error, size_t size)
{
    int w = S.want_ext_w, h = S.want_ext_h;
    if (w <= 0 || h <= 0)
        if (!uwp_window_pixels(&w, &h) || w <= 0 || h <= 0)
            w = S.canvas_w, h = S.canvas_h;

    ComPtr<IDXGIDevice1> dxgi_device;
    CHECK(S.device.As(&dxgi_device), "IDXGIDevice1");
    dxgi_device->SetMaximumFrameLatency(1);
    ComPtr<IDXGIAdapter> adapter;
    CHECK(dxgi_device->GetAdapter(adapter.GetAddressOf()), "adapter");
    ComPtr<IDXGIFactory2> factory;
    CHECK(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())), "factory");

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = (UINT)w;
    desc.Height = (UINT)h;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    auto window = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread();
    CHECK(factory->CreateSwapChainForCoreWindow(S.device.Get(), static_cast<::IUnknown *>(winrt::get_abi(window)), &desc,
                                                nullptr, S.swapchain.GetAddressOf()),
          "CreateSwapChainForCoreWindow");
    S.width = w;
    S.height = h;
    return make_target(error, size);
}

// The refresh rate as the present itself keeps it: the average time between presents over 120 of them, taken only while
// each one waited for the screen (a present that came back at once, as when the window is hidden, is not a refresh).
void measure_refresh(HRESULT hr)
{
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    const uint64_t us = (uint64_t)((double)now.QuadPart * 1000000.0 / (double)freq.QuadPart);
    const uint64_t gap = S.last_present_us ? us - S.last_present_us : 0;
    S.last_present_us = us;
    if (hr != S_OK || gap < 3000 || gap > 60000)
    {
        S.interval_sum_us = 0;
        S.interval_count = 0;
        return;
    }
    S.interval_sum_us += gap;
    if (++S.interval_count < 120)
        return;
    const double hz = 1000000.0 * S.interval_count / (double)S.interval_sum_us;
    S.interval_sum_us = 0;
    S.interval_count = 0;
    if (S.measured_hz <= 0 || hz < S.measured_hz * 0.98 || hz > S.measured_hz * 1.02)
        SwanStationPS5_log("d3d11: the present keeps %.2f Hz (the system says %.2f)", hz, S.mode_hz);
    S.measured_hz = hz;
    S.hz = hz;
    snprintf(S.description, sizeof(S.description), "%dx%d @ %.2f Hz", S.width, S.height, S.hz);
}

ID3D11SamplerState *sampler_for(const Texture &t)
{
    return t.smooth ? (t.mips ? S.linear_mip.Get() : S.linear_flat.Get()) : S.point.Get();
}

int add_texture(const uint8_t *rgba, int w, int h, bool smooth, bool mips)
{
    Texture t;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = mips ? 0 : 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (mips ? D3D11_BIND_RENDER_TARGET : 0);
    d.MiscFlags = mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
    D3D11_SUBRESOURCE_DATA init = {rgba, (UINT)w * 4, 0};
    if (FAILED(S.device->CreateTexture2D(&d, mips ? nullptr : &init, t.texture.GetAddressOf())))
        return -1;
    if (FAILED(S.device->CreateShaderResourceView(t.texture.Get(), nullptr, t.view.GetAddressOf())))
        return -1;
    if (mips)
    {
        S.context->UpdateSubresource(t.texture.Get(), 0, nullptr, rgba, (UINT)w * 4, 0);
        S.context->GenerateMips(t.view.Get());
    }
    t.width = w;
    t.height = h;
    t.smooth = smooth;
    t.mips = mips;
    t.used = true;
    for (size_t i = 0; i < S.textures.size(); ++i)
        if (!S.textures[i].used)
        {
            S.textures[i] = std::move(t);
            return (int)i;
        }
    S.textures.push_back(std::move(t));
    return (int)S.textures.size() - 1;
}

void push(bool game, int texture, UINT first, UINT count)
{
    Command c = {};
    c.game = game;
    c.texture = texture;
    c.blend = S.blend;
    c.clipped = S.clipped;
    memcpy(c.clip, S.clip, sizeof(c.clip));
    c.first = first;
    c.count = count;
    S.commands.push_back(c);
}

void quad(float x, float y, float w, float h, float v0, float v1, uint32_t argb, std::vector<DxpUiVertex> &out)
{
    const float x1 = x + w, y1 = y + h;
    out.insert(out.end(), {{x, y, 0, v0, argb}, {x1, y, 1, v0, argb}, {x1, y1, 1, v1, argb},
                           {x, y, 0, v0, argb}, {x1, y1, 1, v1, argb}, {x, y1, 0, v1, argb}});
}
} // namespace

extern "C"
{
void dxp_want_size(int, int) {}
void dxp_want_extent(int width, int height)
{
    S.want_ext_w = width;
    S.want_ext_h = height;
}
void dxp_want_high_refresh(bool) {}
const char *dxp_describe(void) { return S.description; }
double dxp_display_hz(void) { return S.hz; }

bool dxp_open(int canvas_w, int canvas_h, char *error, size_t size)
{
    S.canvas_w = canvas_w;
    S.canvas_h = canvas_h;
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                   D3D11_SDK_VERSION, S.device.GetAddressOf(), &got, S.context.GetAddressOf());
    if (FAILED(hr))
    {
        snprintf(error, size, "D3D11CreateDevice failed (0x%08lx)", (unsigned long)hr);
        return false;
    }
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(S.device.As(&mt)))
        mt->SetMultithreadProtected(TRUE);
    SwanStationPS5_log("d3d11: device created, feature level 0x%x", (unsigned)got);
    if (!make_swapchain(error, size))
        return false;
    describe();
    SwanStationPS5_log("d3d11: swap chain %dx%d", S.width, S.height);

    memset(&S.core, 0, sizeof(S.core));
    S.core.interface_type = RETRO_HW_RENDER_INTERFACE_D3D11;
    S.core.interface_version = RETRO_HW_RENDER_INTERFACE_D3D11_VERSION;
    S.core.handle = &S;
    S.core.device = S.device.Get();
    S.core.context = S.context.Get();
    S.core.featureLevel = got;
    S.core.D3DCompile = nullptr; // SwanStation's D3D11 shaders are built into the core
    return true;
}

bool dxp_ui_open(char *error, size_t size)
{
    CHECK(S.device->CreateVertexShader(g_ui_vs, sizeof(g_ui_vs), nullptr, S.vs.GetAddressOf()), "vertex shader");
    CHECK(S.device->CreatePixelShader(g_ui_ps, sizeof(g_ui_ps), nullptr, S.ps.GetAddressOf()), "pixel shader");
    static const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    CHECK(S.device->CreateInputLayout(elements, 3, g_ui_vs, sizeof(g_ui_vs), S.layout.GetAddressOf()), "input layout");

    const float canvas[4] = {(float)S.canvas_w, (float)S.canvas_h, 0, 0};
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(canvas);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init = {canvas, 0, 0};
    CHECK(S.device->CreateBuffer(&bd, &init, S.constants.GetAddressOf()), "constant buffer");

    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    CHECK(S.device->CreateBlendState(&blend, S.blend_on.GetAddressOf()), "blend state");
    blend.RenderTarget[0].BlendEnable = FALSE;
    CHECK(S.device->CreateBlendState(&blend, S.blend_off.GetAddressOf()), "blend state");

    D3D11_RASTERIZER_DESC rs = {};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.ScissorEnable = TRUE;
    rs.DepthClipEnable = TRUE;
    CHECK(S.device->CreateRasterizerState(&rs, S.rasterizer.GetAddressOf()), "rasterizer state");
    D3D11_DEPTH_STENCIL_DESC ds = {};
    CHECK(S.device->CreateDepthStencilState(&ds, S.depth.GetAddressOf()), "depth state");

    D3D11_SAMPLER_DESC sd = {};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    CHECK(S.device->CreateSamplerState(&sd, S.linear_mip.GetAddressOf()), "sampler");
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    CHECK(S.device->CreateSamplerState(&sd, S.linear_flat.GetAddressOf()), "sampler");
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    CHECK(S.device->CreateSamplerState(&sd, S.point.GetAddressOf()), "sampler");

    static const uint8_t white[4] = {255, 255, 255, 255};
    S.white = add_texture(white, 1, 1, false, false);
    if (S.white < 0)
    {
        snprintf(error, size, "the interface's white texture could not be made");
        return false;
    }
    // the game's shaders: one vertex shader (a rectangle from six vertex ids), the pixel shaders of the single-pass ones
    CHECK(S.device->CreateVertexShader(SPV_QUAD_VERT, sizeof(SPV_QUAD_VERT), nullptr, S.game_vs.GetAddressOf()), "game vertex shader");
    static const struct { const unsigned char *code; size_t size; } game_code[6] = {
        {SPV_QUAD_FRAG, sizeof(SPV_QUAD_FRAG)},         {SPV_LCD3X_FRAG, sizeof(SPV_LCD3X_FRAG)},
        {SPV_DOWNSAMPLE_FRAG, sizeof(SPV_DOWNSAMPLE_FRAG)}, {SPV_CRT_LOTTES_FRAG, sizeof(SPV_CRT_LOTTES_FRAG)},
        {SPV_CRT_PI_FRAG, sizeof(SPV_CRT_PI_FRAG)},     {SPV_ZFAST_LCD_FRAG, sizeof(SPV_ZFAST_LCD_FRAG)}};
    for (int k = 0; k < 6; ++k)
        CHECK(S.device->CreatePixelShader(game_code[k].code, game_code[k].size, nullptr, S.game_ps[k].GetAddressOf()), "game pixel shader");
    S.game_push = nullptr;
    {
        D3D11_BUFFER_DESC gb = {};
        gb.ByteWidth = 64;
        gb.Usage = D3D11_USAGE_DEFAULT;
        gb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        CHECK(S.device->CreateBuffer(&gb, nullptr, S.game_push.GetAddressOf()), "game constants");
    }
    S.chain = dxc_create(S.device.Get(), S.context.Get());
    S.fsr_chain = dxc_create(S.device.Get(), S.context.Get());
    S.ui_ready = true;
    return true;
}

bool dxp_ui_active(void) { return S.ui_ready; }

void dxp_close(void)
{
    dxc_destroy(S.chain);
    dxc_destroy(S.fsr_chain);
    S.chain = S.fsr_chain = nullptr;
    S.game_view.Reset();
    S.textures.clear();
    S.ui_ready = false;
    if (S.context)
        S.context->ClearState();
    S.target.Reset();
    S.swapchain.Reset();
    S.context.Reset();
    S.device.Reset();
}

bool dxp_resize(int width, int height, char *error, size_t size)
{
    if (!S.swapchain || width <= 0 || height <= 0 || (width == S.width && height == S.height))
        return true;
    S.context->OMSetRenderTargets(0, nullptr, nullptr);
    S.target.Reset();
    CHECK(S.swapchain->ResizeBuffers(0, (UINT)width, (UINT)height, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers");
    S.width = width;
    S.height = height;
    describe();
    SwanStationPS5_log("d3d11: swap chain resized to %dx%d", width, height);
    return make_target(error, size);
}

bool dxp_set_mode(int, int, char *, size_t)
{
    describe();
    return true;
}

int dxp_ui_texture_create(const uint8_t *rgba, int w, int h, bool smooth, bool mips, bool)
{
    return S.ui_ready ? add_texture(rgba, w, h, smooth, mips) : -1;
}

void dxp_ui_texture_free(int id)
{
    if (id >= 0 && id < (int)S.textures.size() && id != S.white)
        S.textures[id] = Texture();
}

void dxp_ui_texture_update(int id, const uint8_t *rgba)
{
    if (id < 0 || id >= (int)S.textures.size() || !S.textures[id].used)
        return;
    Texture &t = S.textures[id];
    S.context->UpdateSubresource(t.texture.Get(), 0, nullptr, rgba, (UINT)t.width * 4, 0);
    if (t.mips)
        S.context->GenerateMips(t.view.Get());
}

void dxp_ui_blend(bool on) { S.blend = on; }

void dxp_ui_begin(uint32_t clear_argb)
{
    const bool set = (clear_argb >> 24) != 0;
    S.clear[0] = set ? ((clear_argb >> 16) & 0xff) / 255.0f : 0.0f;
    S.clear[1] = set ? ((clear_argb >> 8) & 0xff) / 255.0f : 0.0f;
    S.clear[2] = set ? (clear_argb & 0xff) / 255.0f : 0.0f;
    S.clear[3] = 1.0f;
    S.list.clear();
    S.commands.clear();
    S.blend = true;
    S.clipped = false;
    S.game_shown = false;
}

void dxp_ui_clip(int x, int y, int w, int h)
{
    S.clipped = w > 0 && h > 0;
    S.clip[0] = x;
    S.clip[1] = y;
    S.clip[2] = w;
    S.clip[3] = h;
}

void dxp_ui_draw(int id, const DxpUiVertex *v, int count)
{
    if (!S.ui_ready || count < 3)
        return;
    const int tex = id >= 0 ? id : S.white;
    const UINT first = (UINT)S.list.size();
    S.list.insert(S.list.end(), v, v + count);
    // consecutive draws with the same state are one
    if (!S.commands.empty())
    {
        Command &c = S.commands.back();
        if (!c.game && c.texture == tex && c.blend == S.blend && c.clipped == S.clipped &&
            (!c.clipped || !memcmp(c.clip, S.clip, sizeof(c.clip))) && c.first + c.count == first)
        {
            c.count += (UINT)count;
            return;
        }
    }
    push(false, tex, first, (UINT)count);
}

void dxp_ui_game_here(void)
{
    if (S.game_view)
        push(true, -1, 0, 0);
}

bool dxp_game_image_ready(void) { return S.game_view != nullptr; }
void dxp_set_game_nearest(bool nearest) { S.game_nearest = nearest; }

void dxp_show_game(float x, float y, float w, float h, float crop, int shader, int tex_w, int tex_h, int lines)
{
    S.game_rect[0] = x;
    S.game_rect[1] = y;
    S.game_rect[2] = w;
    S.game_rect[3] = h;
    S.game_crop = crop;
    S.game_shader = shader;
    S.game_tex_w = tex_w;
    S.game_tex_h = tex_h;
    S.game_lines = lines;
    S.game_shown = true;
}

namespace
{
// The values of Settings > Display > Shader
const int SHADER_ROYALE = 2, SHADER_NTSC_FIRST = 5, SHADER_NTSC_LAST = 8, SHADER_CRT_FIRST = 13, SHADER_CRT_LAST = 16;

// The chain a shader value runs: 0 CRT Royale Fast, 1 to 4 the NTSC presets, 6 to 9 the presets of shaders_crt.h; -1 for
// the others.
int chain_of_shader(int sh)
{
    if (sh == SHADER_ROYALE)
        return 0;
    if (sh >= SHADER_CRT_FIRST && sh <= SHADER_CRT_LAST)
        return sh - SHADER_CRT_FIRST + 6;
    return sh >= SHADER_NTSC_FIRST && sh <= SHADER_NTSC_LAST ? sh - SHADER_NTSC_FIRST + 1 : -1;
}

// The pixel shader of each single-pass shader value: 0 the plain picture, 1 LCD3x, 2 supersampling (99), then 9, 10, 12
int game_shader_slot(int sh)
{
    return sh == 1 ? 1 : sh == 99 ? 2 : sh == 9 ? 3 : sh == 10 ? 4 : sh == 12 ? 5 : 0;
}

// The game's picture, through its shader, in the rectangle of the canvas the interface left for it. Everything it touches
// of the context is set again afterwards by the caller.
void draw_game(const Command &c, float kx, float ky, const D3D11_RECT &clip)
{
    ID3D11DeviceContext *cx = S.context.Get();
    const float x = S.game_rect[0] * kx, y = S.game_rect[1] * ky, w = S.game_rect[2] * kx, h = S.game_rect[3] * ky;
    const float crop = S.game_crop, shown = 1.0f - 2.0f * crop;
    const int rect_w = (int)(w + 0.5f), rect_h = (int)(h + 0.5f);
    ID3D11ShaderResourceView *picture = S.game_view.Get();
    bool scaled = false; // the picture is FSR 1's, already at the size it has on screen

    // FSR 1, before the picture's shader: the core's picture scaled to its size on screen, edge-adaptively and sharpened
    if (S.fsr && S.fsr_chain && rect_w >= 16 && rect_h >= 16 && S.game_w >= 1 && S.game_h >= 1)
    {
        const int out_w = rect_w, out_h = (int)(rect_h / (shown > 0.1f ? shown : 1.0f) + 0.5f);
        const int src_w = (int)S.game_w, src_h = (int)S.game_h;
        if ((src_w < out_w || src_h < out_h) && out_w >= 16 && out_h >= 16 &&
            dxc_prepare(S.fsr_chain, DXC_FSR, true, src_w, src_h, out_w, out_h, src_w, src_h))
        {
            dxc_record(S.fsr_chain, picture, false);
            if (ID3D11ShaderResourceView *out = dxc_output(S.fsr_chain))
            {
                picture = out;
                scaled = true;
            }
        }
    }

    const int sh = S.game_shader;
    const int chain = chain_of_shader(sh);
    const int tex_w = S.game_tex_w > 0 ? S.game_tex_w : (int)S.game_w, tex_h = S.game_tex_h > 0 ? S.game_tex_h : (int)S.game_h;
    bool chained = false;
    if (chain >= 0 && S.chain && tex_w >= 1 && tex_h >= 1 && rect_w >= 1 && rect_h >= 1)
    {
        // the PS1's own picture: its lines, and as many columns as the game's shape gives
        int native_h = S.game_lines >= 1 ? S.game_lines : 240;
        int native_w = (int)((float)tex_w * native_h / tex_h + 0.5f);
        if (native_w < 1) native_w = 1;
        if (native_h < 1) native_h = 1;
        if (dxc_prepare(S.chain, chain, false, native_w, native_h, rect_w, rect_h, tex_w, tex_h))
        {
            dxc_record(S.chain, picture, scaled);
            chained = true;
        }
    }

    // back on the swap chain
    cx->OMSetRenderTargets(1, S.target.GetAddressOf(), nullptr);
    D3D11_VIEWPORT full = {0, 0, (float)S.width, (float)S.height, 0, 1};
    cx->RSSetViewports(1, &full);
    cx->OMSetBlendState(S.blend_off.Get(), nullptr, 0xffffffffu);
    if (chained)
    {
        dxc_final(S.chain, picture, scaled, (int)(x + 0.5f), (int)(y + 0.5f), rect_w, rect_h, S.width, S.height);
        return;
    }

    // the single-pass shaders: the push constants are the rectangle (clip space, y down), the picture's part, the shader's
    // information and the colours (the Display settings)
    float info[4], k[16];
    const float tw = scaled ? w : (float)tex_w, th = scaled ? h / (shown > 0.1f ? shown : 1.0f) : (float)tex_h;
    const bool lines_shader = sh == 1 || sh == 9 || sh == 10 || sh == 12;
    info[0] = tw;
    info[1] = th;
    info[2] = lines_shader ? (float)S.game_lines * shown : w / (tw > 0 ? tw : 1);
    info[3] = h / (th * shown > 0 ? th * shown : 1);
    const float cw = (float)S.width, ch = (float)S.height;
    const float v[16] = {x / cw * 2.0f - 1.0f, y / ch * 2.0f - 1.0f, (x + w) / cw * 2.0f - 1.0f, (y + h) / ch * 2.0f - 1.0f,
                         0.0f, crop, 1.0f, 1.0f - crop, info[0], info[1], info[2], info[3],
                         S.colour[0], S.colour[1], S.colour[2], S.colour[3]};
    memcpy(k, v, sizeof(k));
    cx->UpdateSubresource(S.game_push.Get(), 0, nullptr, k, 0, 0);
    cx->IASetInputLayout(nullptr);
    cx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cx->VSSetShader(S.game_vs.Get(), nullptr, 0);
    cx->PSSetShader(S.game_ps[game_shader_slot(sh)].Get(), nullptr, 0);
    ID3D11Buffer *buffers[2] = {S.constants.Get(), S.game_push.Get()};
    cx->VSSetConstantBuffers(0, 2, buffers);
    cx->PSSetConstantBuffers(0, 2, buffers);
    cx->PSSetShaderResources(0, 1, &picture);
    // sharp pixels, or smooth; FSR 1's picture is exactly the size it has on screen
    cx->PSSetSamplers(0, 1, S.game_nearest || scaled ? S.point.GetAddressOf() : S.linear_flat.GetAddressOf());
    cx->RSSetScissorRects(1, &clip);
    cx->Draw(6, 0);
}
} // namespace

void dxp_present_ui(void)
{
    if (!S.swapchain || !S.ui_ready)
        return;
    ID3D11DeviceContext *cx = S.context.Get();
    cx->OMSetRenderTargets(1, S.target.GetAddressOf(), nullptr);
    cx->ClearRenderTargetView(S.target.Get(), S.clear);

    if (!S.list.empty() || !S.commands.empty())
    {
        const UINT bytes = (UINT)(S.list.size() * sizeof(DxpUiVertex));
        if (bytes > 0 && bytes > S.vertices_capacity)
        {
            S.vertices_capacity = bytes + bytes / 2 + 65536;
            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth = S.vertices_capacity;
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(S.device->CreateBuffer(&bd, nullptr, S.vertices.ReleaseAndGetAddressOf())))
            {
                S.vertices_capacity = 0;
                return;
            }
        }
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(cx->Map(S.vertices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            memcpy(m.pData, S.list.data(), bytes);
            cx->Unmap(S.vertices.Get(), 0);
        }

        // the interface's state: set again after the game's picture, whose shaders change all of it
        auto bind_ui = [&]() {
            D3D11_VIEWPORT vp = {0, 0, (float)S.width, (float)S.height, 0, 1};
            cx->OMSetRenderTargets(1, S.target.GetAddressOf(), nullptr);
            cx->RSSetViewports(1, &vp);
            cx->RSSetState(S.rasterizer.Get());
            cx->OMSetDepthStencilState(S.depth.Get(), 0);
            cx->IASetInputLayout(S.layout.Get());
            cx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const UINT stride = sizeof(DxpUiVertex), offset = 0;
            cx->IASetVertexBuffers(0, 1, S.vertices.GetAddressOf(), &stride, &offset);
            cx->VSSetShader(S.vs.Get(), nullptr, 0);
            cx->VSSetConstantBuffers(0, 1, S.constants.GetAddressOf());
            cx->PSSetShader(S.ps.Get(), nullptr, 0);
        };
        bind_ui();

        const float kx = (float)S.width / S.canvas_w, ky = (float)S.height / S.canvas_h;
        for (const Command &c : S.commands)
        {
            D3D11_RECT r = {0, 0, S.width, S.height};
            if (c.clipped)
            {
                r.left = (LONG)(c.clip[0] * kx);
                r.top = (LONG)(c.clip[1] * ky);
                r.right = (LONG)((c.clip[0] + c.clip[2]) * kx + 0.5f);
                r.bottom = (LONG)((c.clip[1] + c.clip[3]) * ky + 0.5f);
                if (r.left < 0) r.left = 0;
                if (r.top < 0) r.top = 0;
                if (r.right > S.width) r.right = S.width;
                if (r.bottom > S.height) r.bottom = S.height;
                if (r.right <= r.left || r.bottom <= r.top)
                    continue;
            }
            cx->RSSetScissorRects(1, &r);
            cx->OMSetBlendState(c.blend ? S.blend_on.Get() : S.blend_off.Get(), nullptr, 0xffffffffu);
            if (c.game)
            {
                if (S.game_shown && S.game_view)
                {
                    draw_game(c, kx, ky, r);
                    bind_ui();
                }
                continue;
            }
            const Texture &t = S.textures[c.texture];
            ID3D11ShaderResourceView *view = t.view.Get();
            cx->PSSetShaderResources(0, 1, &view);
            ID3D11SamplerState *sampler = sampler_for(t);
            cx->PSSetSamplers(0, 1, &sampler);
            cx->Draw(c.count, c.first);
        }
        ID3D11ShaderResourceView *none = nullptr;
        cx->PSSetShaderResources(0, 1, &none);
    }
    cx->OMSetRenderTargets(0, nullptr, nullptr);

    HRESULT hr = S.swapchain->Present(1, 0);
    measure_refresh(hr);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        SwanStationPS5_log("d3d11: the device was lost (0x%08lx, reason 0x%08lx)", (unsigned long)hr,
                           (unsigned long)S.device->GetDeviceRemovedReason());
}

/* ---------------------------------------------------------------- the core */

const void *dxp_core_interface(void) { return S.device ? &S.core : nullptr; }

void dxp_take_core_frame(unsigned width, unsigned height)
{
    if (!S.context)
        return;
    ComPtr<ID3D11ShaderResourceView> view;
    S.context->PSGetShaderResources(0, 1, view.GetAddressOf());
    if (view)
    {
        S.game_view = view;
        S.game_w = width;
        S.game_h = height;
    }
}

void dxp_drop_core_frame(void)
{
    S.game_view.Reset();
    S.game_w = S.game_h = 0;
}

bool dxp_capture_game(uint8_t *rgba, int w, int h, int src_w, int src_h)
{
    if (!S.game_view || w <= 0 || h <= 0 || src_w <= 0 || src_h <= 0)
        return false;
    ComPtr<ID3D11Resource> resource;
    S.game_view->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> source;
    if (FAILED(resource.As(&source)))
        return false;
    D3D11_TEXTURE2D_DESC d = {};
    source->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.MiscFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MipLevels = 1;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(S.device->CreateTexture2D(&d, nullptr, staging.GetAddressOf())))
        return false;
    S.context->CopyResource(staging.Get(), source.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(S.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
        return false;
    const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
    for (int y = 0; y < h; ++y)
    {
        const unsigned sy = (unsigned)(((int64_t)y * 2 + 1) * src_h / (2 * h));
        const uint8_t *row = (const uint8_t *)m.pData + (size_t)(sy < d.Height ? sy : d.Height - 1) * m.RowPitch;
        for (int x = 0; x < w; ++x)
        {
            const unsigned sx = (unsigned)(((int64_t)x * 2 + 1) * src_w / (2 * w));
            const uint8_t *p = row + (size_t)(sx < d.Width ? sx : d.Width - 1) * 4;
            uint8_t *o = rgba + ((size_t)y * w + x) * 4;
            o[0] = bgra ? p[2] : p[0];
            o[1] = p[1];
            o[2] = bgra ? p[0] : p[2];
            o[3] = 255;
        }
    }
    S.context->Unmap(staging.Get(), 0);
    return true;
}

void dxp_set_colour(float brightness, float saturation, float warmth, float sharpen)
{
    S.colour[0] = brightness;
    S.colour[1] = saturation;
    S.colour[2] = warmth;
    S.colour[3] = sharpen;
}
void dxp_set_fsr(bool on) { S.fsr = on; }
void dxp_set_framegen(int, double, double, bool) {}
int dxp_framegen_times(void) { return 0; }
double dxp_framegen_hz(void) { return 0.0; }
void dxp_frame_repeated(void) {}
}
