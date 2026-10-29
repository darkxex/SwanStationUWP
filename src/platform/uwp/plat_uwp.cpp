/*
 * SwanStationUWP - platform layer for Xbox Series X|S (UWP, developer mode).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The same interface platform.h gave the PS5 and desktop builds, on what UWP offers: the screen through Vulkan (Mesa's
 * Dozen on Direct3D 12, vk_present.c), Xbox controllers through XInput, sound through XAudio2.
 * The interface is always drawn by the GPU (vk_ui.inc); there is no CPU canvas.
 */
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.UserProfile.h>

#include <windows.h>
#include <xaudio2.h>
#include <xinput.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <mutex>
#include <vector>

extern "C"
{
#include "../platform.h"
#include "../../i18n.h"
#include "../vk/vk_present.h"
}
#include "uwp_app.h"


#define SCREEN_W 1920 /* the interface's coordinates; the swap chain can be any size */
#define SCREEN_H 1080

static char init_error[256];
static char screen_info[400] = "Vulkan (Dozen)";
static bool gpu_ui;
static int output_wanted; /* 0 1080p, 1 1440p, 2 4K */
static bool backdrop_dirty;

extern "C" const char *plat_init_error(void) { return init_error; }
extern "C" const char *plat_screen_info(void) { return screen_info; }

/* A textured rectangle of the GPU interface; tex < 0: the tint alone. Corners in canvas pixels. */
static void gpu_quad(int tex, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                     uint32_t argb)
{
    const VkpUiVertex v[6] = {{x0, y0, u0, v0, argb}, {x1, y0, u1, v0, argb}, {x1, y1, u1, v1, argb},
                              {x0, y0, u0, v0, argb}, {x1, y1, u1, v1, argb}, {x0, y1, u0, v1, argb}};
    vkp_ui_draw(tex, v, 6);
}

/* ---------------------------------------------------------------- start and stop */

extern "C" bool plat_init(void)
{
    char error[160];
    static const int OUTPUT_W[] = {1920, 2560, 3840}, OUTPUT_H[] = {1080, 1440, 2160};
    const int output = output_wanted > 0 && output_wanted < 3 ? output_wanted : 0;
    vkp_want_size(OUTPUT_W[output], OUTPUT_H[output]);
    int win_w = 0, win_h = 0;
    if (uwp_window_pixels(&win_w, &win_h))
        vkp_want_extent(win_w, win_h);
    if (!vkp_open(SCREEN_W, SCREEN_H, error, sizeof(error)))
    {
        snprintf(init_error, sizeof(init_error), "screen: %s", error);
        SwanStationPS5_log("%s", init_error);
        return false;
    }
    snprintf(screen_info, sizeof(screen_info), "Vulkan (Dozen), %s", vkp_describe());
    if (!vkp_ui_open(error, sizeof(error)))
    {
        snprintf(init_error, sizeof(init_error), "screen: the interface needs the GPU: %s", error);
        SwanStationPS5_log("%s", init_error);
        return false;
    }
    gpu_ui = true;
    return true;
}

extern "C" void plat_release(void) {}

extern "C" void plat_shutdown(void)
{
    plat_audio_close();
    vkp_close();
    gpu_ui = false;
}

extern "C" void plat_default_root(char *out, size_t size)
{
    // LocalState\SwanStation: reachable from the Device Portal (File explorer) and from a PC over the network
    path_join(out, size, uwp_local_folder(), "SwanStation");
}

extern "C" const char *plat_sandbox_probe(void) { return ""; }
extern "C" bool plat_unlock_disabled(void) { return true; }
extern "C" void plat_set_unlock_disabled(bool) {}
extern "C" bool plat_prepare_storage(char *, size_t) { return true; }

/* ---------------------------------------------------------------- input */

static int player_order[SwanStationPS5_MAX_PADS] = {0, 1, 2, 3}; /* order[player] = the controller */

extern "C" void plat_set_player_order(const int order[SwanStationPS5_MAX_PADS])
{
    memcpy(player_order, order, sizeof(player_order));
}

extern "C" void plat_player_order(int order[SwanStationPS5_MAX_PADS])
{
    memcpy(order, player_order, sizeof(player_order));
}

static int physical(int port)
{
    return port >= 0 && port < SwanStationPS5_MAX_PADS ? player_order[port] : -1;
}

/* The controllers are read with XInput, as RetroArch's UWP build does (xinput1_4 works in a UWP app, on a PC and on an
 * Xbox): slot i is physical controller i. A slot with nothing in it is only asked about once a second, since asking
 * for an empty one every frame is slow. */
static bool pad_present[SwanStationPS5_MAX_PADS];
static uint64_t pad_probe_at[SwanStationPS5_MAX_PADS];
static uint16_t rumble_strong[SwanStationPS5_MAX_PADS], rumble_weak[SwanStationPS5_MAX_PADS];
static int battery[SwanStationPS5_MAX_PADS] = {-1, -1, -1, -1};
static uint64_t battery_read_at[SwanStationPS5_MAX_PADS];

static int clamp16(int v)
{
    return v > 32767 ? 32767 : v < -32767 ? -32767 : v;
}

/* Xbox buttons on the frontend's pad bits (RetroPad numbering, PlayStation names: A is the bottom button, Cross).
 * View (Back) plays the part the DualSense touchpad had (hold it for the shortcuts, tap it for Select); Menu is Start. */
static uint32_t buttons_of(WORD b)
{
    uint32_t out = 0;
    if (b & XINPUT_GAMEPAD_A) out |= BIT(BTN_CROSS);
    if (b & XINPUT_GAMEPAD_B) out |= BIT(BTN_CIRCLE);
    if (b & XINPUT_GAMEPAD_X) out |= BIT(BTN_SQUARE);
    if (b & XINPUT_GAMEPAD_Y) out |= BIT(BTN_TRIANGLE);
    if (b & XINPUT_GAMEPAD_BACK) out |= BIT(BTN_MENU);
    if (b & XINPUT_GAMEPAD_START) out |= BIT(BTN_START);
    if (b & XINPUT_GAMEPAD_DPAD_UP) out |= BIT(BTN_UP);
    if (b & XINPUT_GAMEPAD_DPAD_DOWN) out |= BIT(BTN_DOWN);
    if (b & XINPUT_GAMEPAD_DPAD_LEFT) out |= BIT(BTN_LEFT);
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT) out |= BIT(BTN_RIGHT);
    if (b & XINPUT_GAMEPAD_LEFT_SHOULDER) out |= BIT(BTN_L1);
    if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) out |= BIT(BTN_R1);
    if (b & XINPUT_GAMEPAD_LEFT_THUMB) out |= BIT(BTN_L3);
    if (b & XINPUT_GAMEPAD_RIGHT_THUMB) out |= BIT(BTN_R3);
    return out;
}

extern "C" void plat_poll(PadState pads[SwanStationPS5_MAX_PADS], bool *quit)
{
    uwp_pump_events();
    if (quit && uwp_quit_requested())
        *quit = true;
    // The window's SizeChanged event, and a look every half second in case it does not come (the console's TV modes)
    static unsigned polls;
    const bool resized = uwp_take_window_resized();
    if (gpu_ui && (resized || ++polls % 30 == 0))
    {
        int w = 0, h = 0;
        if (uwp_window_pixels(&w, &h))
        {
            char error[160];
            if (!vkp_resize(w, h, error, sizeof(error)))
                SwanStationPS5_log("screen: %s", error);
        }
    }
    PadState raw[SwanStationPS5_MAX_PADS];
    memset(raw, 0, sizeof(raw));
    uint64_t now = plat_ticks_us();
    for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
    {
        if (!pad_present[i] && pad_probe_at[i] && now - pad_probe_at[i] < 1000000ull)
            continue;
        pad_probe_at[i] = now;
        XINPUT_STATE st;
        memset(&st, 0, sizeof(st));
        bool found = XInputGetState((DWORD)i, &st) == ERROR_SUCCESS;
        if (found != pad_present[i])
        {
            pad_present[i] = found;
            SwanStationPS5_log("input: controller %d %s", i + 1, found ? "connected" : "disconnected");
        }
        if (!found)
            continue;
        const XINPUT_GAMEPAD &g = st.Gamepad;
        PadState *p = &raw[i];
        p->connected = true;
        p->buttons = buttons_of(g.wButtons);
        p->lx = (int16_t)clamp16(g.sThumbLX);
        p->ly = (int16_t)clamp16(-(int)g.sThumbLY); /* the frontend counts down as positive */
        p->rx = (int16_t)clamp16(g.sThumbRX);
        p->ry = (int16_t)clamp16(-(int)g.sThumbRY);
        p->l2 = g.bLeftTrigger;
        p->r2 = g.bRightTrigger;
        if (p->l2 > 127) p->buttons |= BIT(BTN_L2);
        if (p->r2 > 127) p->buttons |= BIT(BTN_R2);
    }
    for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
        pads[i] = raw[player_order[i]];
}

extern "C" const char *plat_pad_name(int port)
{
    static char name[SwanStationPS5_MAX_PADS][32];
    int i = physical(port);
    if (i < 0 || i >= SwanStationPS5_MAX_PADS || !pad_present[i])
        return NULL;
    snprintf(name[i], sizeof(name[i]), "Xbox Controller %d", i + 1);
    return name[i];
}


extern "C" int plat_pad_battery(int port)
{
    int i = physical(port);
    if (i < 0 || i >= SwanStationPS5_MAX_PADS)
        return -1;
    uint64_t now = plat_ticks_us();
    if (battery_read_at[i] && now - battery_read_at[i] < 10000000ull)
        return battery[i];
    battery_read_at[i] = now;
    battery[i] = -1;
    XINPUT_BATTERY_INFORMATION info;
    memset(&info, 0, sizeof(info));
    /* XInput only gives four levels; a wired pad has no battery to tell about */
    if (pad_present[i] && XInputGetBatteryInformation((DWORD)i, BATTERY_DEVTYPE_GAMEPAD, &info) == ERROR_SUCCESS &&
        info.BatteryType != BATTERY_TYPE_DISCONNECTED && info.BatteryType != BATTERY_TYPE_WIRED &&
        info.BatteryType != BATTERY_TYPE_UNKNOWN)
    {
        switch (info.BatteryLevel)
        {
        case BATTERY_LEVEL_EMPTY: battery[i] = 5; break;
        case BATTERY_LEVEL_LOW: battery[i] = 25; break;
        case BATTERY_LEVEL_MEDIUM: battery[i] = 60; break;
        default: battery[i] = 100; break;
        }
    }
    return battery[i];
}

extern "C" void plat_rumble(int port, uint16_t strong, uint16_t weak)
{
    int i = physical(port);
    if (i < 0 || i >= SwanStationPS5_MAX_PADS)
        return;
    if (rumble_strong[i] == strong && rumble_weak[i] == weak)
        return;
    rumble_strong[i] = strong;
    rumble_weak[i] = weak;
    if (!pad_present[i])
        return;
    XINPUT_VIBRATION v;
    v.wLeftMotorSpeed = strong;
    v.wRightMotorSpeed = weak;
    XInputSetState((DWORD)i, &v);
}

/* ---------------------------------------------------------------- audio (XAudio2) */

namespace
{
struct Block
{
    size_t frames;
    int16_t samples[1];
};

struct AudioCallback : IXAudio2VoiceCallback
{
    volatile LONG64 queued = 0; /* frames submitted and not yet played */
    void STDMETHODCALLTYPE OnBufferEnd(void *context) override
    {
        Block *b = (Block *)context;
        InterlockedAdd64(&queued, -(LONG64)b->frames);
        free(b);
    }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void *) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void *) override {}
    void STDMETHODCALLTYPE OnVoiceError(void *, HRESULT) override {}
};
} // namespace

static IXAudio2 *xaudio;
static IXAudio2MasteringVoice *master;
static IXAudio2SourceVoice *voice;
static AudioCallback audio_callback;
static int audio_rate;
static double audio_ratio = 1.0, audio_pos; /* the sound stretched by a hair (linear) for Video sync */
static int16_t audio_last[2];

extern "C" bool plat_audio_open(int sample_rate)
{
    if (voice && audio_rate == sample_rate)
    {
        plat_audio_clear();
        return true;
    }
    plat_audio_close();
    if (!xaudio)
    {
        if (FAILED(XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(xaudio->CreateMasteringVoice(&master)))
        {
            SwanStationPS5_log("audio: XAudio2 could not start");
            if (xaudio)
                xaudio->Release();
            xaudio = NULL;
            return false;
        }
    }
    WAVEFORMATEX fmt = {};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = (DWORD)sample_rate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * 4;
    if (FAILED(xaudio->CreateSourceVoice(&voice, &fmt, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &audio_callback)))
    {
        SwanStationPS5_log("audio: no voice at %d Hz", sample_rate);
        voice = NULL;
        return false;
    }
    audio_rate = sample_rate;
    voice->Start(0);
    SwanStationPS5_log("audio: %d Hz", sample_rate);
    return true;
}

extern "C" void plat_audio_close(void)
{
    if (voice)
    {
        voice->Stop(0);
        voice->FlushSourceBuffers();
        voice->DestroyVoice();
        voice = NULL;
    }
    audio_rate = 0;
}

static void audio_submit(const int16_t *frames, size_t count)
{
    Block *b = (Block *)malloc(sizeof(Block) + count * 4);
    if (!b)
        return;
    b->frames = count;
    memcpy(b->samples, frames, count * 4);
    XAUDIO2_BUFFER buffer = {};
    buffer.AudioBytes = (UINT32)(count * 4);
    buffer.pAudioData = (const BYTE *)b->samples;
    buffer.pContext = b;
    InterlockedAdd64(&audio_callback.queued, (LONG64)count);
    if (FAILED(voice->SubmitSourceBuffer(&buffer)))
    {
        InterlockedAdd64(&audio_callback.queued, -(LONG64)count);
        free(b);
    }
}

extern "C" void plat_audio_set_ratio(double ratio) { audio_ratio = ratio; }

extern "C" void plat_audio_push(const int16_t *frames, size_t frame_count)
{
    if (!voice || !frame_count)
        return;
    if (audio_ratio > 0.9999 && audio_ratio < 1.0001)
    {
        audio_pos = 0;
        audio_last[0] = frames[frame_count * 2 - 2];
        audio_last[1] = frames[frame_count * 2 - 1];
        audio_submit(frames, frame_count);
        return;
    }
    /* s[0] is the last frame of the block before, s[1..n] this block's; out[k] is read at audio_pos + k * ratio */
    static int16_t *out;
    static size_t out_cap;
    size_t need = (size_t)((double)frame_count / audio_ratio) + 3;
    if (need > out_cap)
    {
        free(out);
        out = (int16_t *)malloc(need * 4);
        out_cap = out ? need : 0;
        if (!out)
            return;
    }
    size_t n = 0;
    double p = audio_pos;
    while (p < (double)frame_count && n < out_cap)
    {
        size_t i = (size_t)p;
        double f = p - (double)i;
        for (int c = 0; c < 2; ++c)
        {
            double a = i == 0 ? audio_last[c] : frames[(i - 1) * 2 + c];
            double b = frames[i * 2 + c];
            out[n * 2 + c] = (int16_t)(a + (b - a) * f);
        }
        ++n;
        p += audio_ratio;
    }
    audio_pos = p - (double)frame_count;
    audio_last[0] = frames[frame_count * 2 - 2];
    audio_last[1] = frames[frame_count * 2 - 1];
    if (n)
        audio_submit(out, n);
}

extern "C" size_t plat_audio_queued_frames(void)
{
    LONG64 q = voice ? audio_callback.queued : 0;
    return q > 0 ? (size_t)q : 0;
}

extern "C" void plat_audio_clear(void)
{
    if (voice)
        voice->FlushSourceBuffers(); /* OnBufferEnd runs for each flushed buffer, which takes them off the count */
}

/* ---------------------------------------------------------------- screen */

extern "C" int plat_width(void) { return SCREEN_W; }
extern "C" int plat_height(void) { return SCREEN_H; }

extern "C" void plat_begin_frame(uint32_t clear_argb) { vkp_ui_begin(clear_argb); }

static float colour_k[3] = {1.0f, 1.0f, 0.0f}; /* brightness, saturation, warmth */

extern "C" void plat_set_colour(int brightness, int colour, int sharpen)
{
    static int last_b = -1, last_c = -1, last_s = -1;
    if (brightness == last_b && colour == last_c && sharpen == last_s)
        return;
    last_b = brightness;
    last_c = colour;
    last_s = sharpen;
    static const float bright[] = {0.85f, 1.0f, 1.12f, 1.25f};
    static const float sat[] = {1.0f, 1.3f, 0.78f, 1.05f, 1.0f, 0.0f};
    static const float warm[] = {0.0f, 0.0f, 0.0f, 0.06f, -0.06f, 0.0f};
    colour_k[0] = bright[brightness >= 0 && brightness < 4 ? brightness : 1];
    colour_k[1] = sat[colour >= 0 && colour < 6 ? colour : 0];
    colour_k[2] = warm[colour >= 0 && colour < 6 ? colour : 0];
    static const float sharp[] = {0.0f, 0.45f, 1.0f};
    vkp_set_colour(colour_k[0], colour_k[1], colour_k[2], sharp[sharpen >= 0 && sharpen < 3 ? sharpen : 0]);
}

extern "C" void plat_set_fsr(bool on) { vkp_set_fsr(on); }
extern "C" double plat_display_hz(void) { return vkp_display_hz(); }
extern "C" double plat_framegen_hz(void) { return vkp_framegen_hz(); }
extern "C" int plat_framegen_times(void) { return vkp_framegen_times(); }
extern "C" void plat_set_framegen(int generated, double core_hz, double speed, bool nominal)
{
    vkp_set_framegen(generated, core_hz, speed, nominal);
}
extern "C" void plat_want_output(int resolution) { output_wanted = resolution; }
extern "C" bool plat_open_file_access_settings(void) { return uwp_open_file_access_settings(); }
extern "C" bool plat_gpu_ui_active(void) { return gpu_ui; }

extern "C" bool plat_set_output(int resolution)
{
    static const int OUTPUT_W[] = {1920, 2560, 3840}, OUTPUT_H[] = {1080, 1440, 2160};
    if (!gpu_ui || resolution < 0 || resolution > 2)
        return false;
    // The TV first (the swap chain follows the window, which the console resizes to the TV's mode), then the screen's objects
    int w = 0, h = 0;
    double hz = 0;
    if (uwp_current_display_mode(&w, &h, &hz) && (w != OUTPUT_W[resolution] || h != OUTPUT_H[resolution]))
        uwp_request_display_mode(OUTPUT_W[resolution], OUTPUT_H[resolution], hz > 0 ? hz : 60.0);
    char error[160];
    if (vkp_set_mode(OUTPUT_W[resolution], OUTPUT_H[resolution], error, sizeof(error)))
        return true;
    SwanStationPS5_log("screen: %dx%d failed: %s", OUTPUT_W[resolution], OUTPUT_H[resolution], error);
    return false;
}

extern "C" void plat_want_high_refresh(bool on)
{
    vkp_want_high_refresh(on);
    int w = 0, h = 0;
    double hz = 0;
    if (!uwp_current_display_mode(&w, &h, &hz))
        return;
    double want = on ? 120.0 : 60.0;
    if (hz < want - 0.5 || hz > want + 0.5)
        if (!uwp_request_display_mode(w, h, want))
            SwanStationPS5_log("screen: the TV has no %dx%d mode at %.0f Hz", w, h, want);
}

/* ---------------------------------------------------------------- game picture */

static bool game_gpu; /* the core renders through Vulkan: the screen draws its image */
static int game_src_w = 320, game_src_h = 240;
static int soft_tex = -1, soft_w, soft_h;
static bool soft_smooth;
static uint8_t *soft_rgba; /* the software renderer's picture, R,G,B,A bytes */
static int game_rect[4];

extern "C" void plat_upload_game_gpu(int width, int height)
{
    game_gpu = true;
    game_src_w = width;
    game_src_h = height;
}

/* The core drew on the CPU (the Software renderer): the frame goes into a texture of the GPU interface. The CPU
 * prescalers of the PS5 build (Scale2x, xBR) are left to the GPU's own filtering. */
extern "C" void plat_upload_game(const void *pixels, int width, int height, size_t pitch, int pixel_format, int upscale,
                                 int filter)
{
    (void)upscale, (void)filter;
    if (!pixels || width <= 0 || height <= 0)
        return;
    game_gpu = false;
    game_src_w = width;
    game_src_h = height;
    if (soft_tex < 0 || soft_w != width || soft_h != height)
    {
        if (soft_tex >= 0)
            vkp_ui_texture_free(soft_tex);
        free(soft_rgba);
        soft_rgba = (uint8_t *)calloc((size_t)width * height, 4);
        soft_tex = -1;
        if (soft_rgba)
            soft_tex = vkp_ui_texture_create(soft_rgba, width, height, true, false, true);
        soft_w = width;
        soft_h = height;
    }
    if (soft_tex < 0 || !soft_rgba)
        return;
    for (int y = 0; y < height; ++y)
    {
        const uint8_t *row = (const uint8_t *)pixels + (size_t)y * pitch;
        uint8_t *dst = soft_rgba + (size_t)y * width * 4;
        for (int x = 0; x < width; ++x, dst += 4)
        {
            if (pixel_format == 1) /* XRGB8888 */
            {
                uint32_t c = ((const uint32_t *)row)[x];
                dst[0] = (uint8_t)(c >> 16), dst[1] = (uint8_t)(c >> 8), dst[2] = (uint8_t)c;
            }
            else if (pixel_format == 2) /* RGB565 */
            {
                uint16_t c = ((const uint16_t *)row)[x];
                dst[0] = (uint8_t)((c >> 11) * 255 / 31), dst[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63), dst[2] = (uint8_t)((c & 31) * 255 / 31);
            }
            else /* 0RGB1555 */
            {
                uint16_t c = ((const uint16_t *)row)[x];
                dst[0] = (uint8_t)(((c >> 10) & 31) * 255 / 31), dst[1] = (uint8_t)(((c >> 5) & 31) * 255 / 31), dst[2] = (uint8_t)((c & 31) * 255 / 31);
            }
            dst[3] = 255;
        }
    }
    vkp_ui_texture_update(soft_tex, soft_rgba);
}

extern "C" void plat_game_rect(int *x, int *y, int *w, int *h)
{
    *x = game_rect[0];
    *y = game_rect[1];
    *w = game_rect[2];
    *h = game_rect[3];
}

extern "C" void plat_fill_rect(int x, int y, int w, int h, uint32_t argb)
{
    if ((argb >> 24) != 0 && w > 0 && h > 0)
        gpu_quad(-1, (float)x, (float)y, (float)(x + w), (float)(y + h), 0, 0, 0, 0, argb);
}

extern "C" void plat_draw_game(const Settings *settings, float display_aspect, uint8_t dim)
{
    const bool hw = game_gpu && vkp_game_image_ready();
    if (!hw && (game_gpu || soft_tex < 0))
        return;
    /* Scanlines the game actually has: interlaced (480) and 2x-internal frames show at the same size as 240-line ones. */
    int lines = game_src_h;
    while (lines > 288)
        lines /= 2;
    /* Crop black edges: the share of the picture's height cut at the top and at the bottom (8 or 16 of the PS1's 240 lines) */
    const float crop =
        (!settings->manual_crop && settings->crop_edges > 0 && settings->crop_edges < 3 ? settings->crop_edges * 8 : 0) / 240.0f;
    const int out_w = SCREEN_W, out_h = SCREEN_H;
    float aspect = display_aspect > 0.0f ? display_aspect : 4.0f / 3.0f;

    int dh = out_h, dw = (int)(out_h * aspect + 0.5f);
    if (dw > out_w)
    {
        dw = out_w;
        dh = (int)(out_w / aspect + 0.5f);
    }
    if (settings->integer_scale)
    {
        int k = dh / (lines > 0 ? lines : 240);
        if (k < 1)
            k = 1;
        dh = lines * k;
        dw = (int)(dh * aspect + 0.5f);
        if (dw > out_w)
            dw = out_w;
    }
    if (settings->border == 2)
    {
        /* TV frame: leave room for the TV around the picture */
        dw = dw * 86 / 100;
        dh = dh * 86 / 100;
    }
    int dx = (out_w - dw) / 2, dy = (out_h - dh) / 2;
    if (settings->manual_crop)
    {
        /* Manual crop: the whole picture in an area the player sets (size in percent, position in thousandths) */
        dw = dw * (settings->mc_w < 25 ? 25 : settings->mc_w > 150 ? 150 : settings->mc_w) / 100;
        dh = dh * (settings->mc_h < 25 ? 25 : settings->mc_h > 150 ? 150 : settings->mc_h) / 100;
        dx = (out_w - dw) / 2 + settings->mc_x * out_w / 1000;
        dy = (out_h - dh) / 2 + settings->mc_y * out_h / 1000;
    }
    game_rect[0] = dx;
    game_rect[1] = dy;
    game_rect[2] = dw;
    game_rect[3] = dh;

    static const uint8_t scan_strength[] = {0, 110, 200};
    const uint8_t scan = scan_strength[settings->crt % 3];

    if (!hw)
    {
        const float v0 = crop, v1 = 1.0f - crop;
        gpu_quad(soft_tex, (float)dx, (float)dy, (float)(dx + dw), (float)(dy + dh), 0.0f, v0, 1.0f, v1,
                 0xff000000u | (uint32_t)dim << 16 | (uint32_t)dim << 8 | dim);
        if (scan && lines > 0)
            for (int y = 0; y < dh; ++y)
            {
                int64_t pos = ((int64_t)y * 2 + 1) * lines * 128 / dh;
                int phase = (int)(pos & 255) - 128;
                uint32_t edge = (uint32_t)(phase * phase) >> 6;
                uint32_t keep = 256 - ((edge * scan) >> 8);
                plat_fill_rect(dx, dy + y, dw, 1, (255 - (keep > 255 ? 255 : keep)) << 24);
            }
        return;
    }
    vkp_ui_game_here(); /* the picture goes in the list here: the border drawn before is under it, the dimming over it */
    // The CPU canvas of the PS5 build is gone: the dimming for menus and the scanlines are black rows over the picture
    if (scan && lines > 0)
        for (int y = 0; y < dh; ++y)
        {
            int64_t pos = ((int64_t)y * 2 + 1) * lines * 128 / dh;
            int phase = (int)(pos & 255) - 128;
            uint32_t edge = (uint32_t)(phase * phase) >> 6;
            uint32_t keep = (uint32_t)dim * (256 - ((edge * scan) >> 8)) >> 8;
            plat_fill_rect(dx, dy + y, dw, 1, (255 - (keep > 255 ? 255 : keep)) << 24);
        }
    else if (dim < 255)
        plat_fill_rect(dx, dy, dw, dh, (uint32_t)(255 - dim) << 24);
    /* supersampling: a picture bigger than its place on screen, averaged down */
    int shader = settings->shader;
    if (!shader && settings->supersampling && (game_src_w > dw || game_src_h > dh))
        shader = 99;
    /* "Smooth final scaling" off: sharp pixels (the shaders need the bilinear sampler) */
    vkp_set_game_nearest(!settings->smooth && !shader);
    vkp_show_game((float)dx, (float)dy, (float)dw, (float)dh, crop, shader, game_src_w, game_src_h, lines);
}

extern "C" void plat_backdrop_changed(void) { backdrop_dirty = true; }

extern "C" void plat_draw_backdrop(const uint8_t *grey, int w, int h, uint32_t tint)
{
    /* the grey picture as a texture, made once and again when the picture is rewritten: the GPU tints and scales it */
    static const uint8_t *shown;
    static int tex = -1, shown_w, shown_h;
    if (tex < 0 || backdrop_dirty || shown != grey || shown_w != w || shown_h != h)
    {
        if (tex >= 0)
            vkp_ui_texture_free(tex);
        uint8_t *px = (uint8_t *)malloc((size_t)w * h * 4);
        tex = -1;
        if (px)
        {
            for (size_t i = 0; i < (size_t)w * h; ++i)
                px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = grey[i], px[i * 4 + 3] = 255;
            tex = vkp_ui_texture_create(px, w, h, true, false, true);
            free(px);
        }
        shown = grey;
        shown_w = w;
        shown_h = h;
        backdrop_dirty = false;
    }
    if (tex >= 0)
        gpu_quad(tex, 0, 0, (float)SCREEN_W, (float)SCREEN_H, 0, 0, 1, 1, 0xff000000u | (tint & 0x00ffffffu));
}

extern "C" void plat_profile(const char *) {}

extern "C" void plat_end_frame(void)
{
    vkp_present_ui(); /* the interface's list over the game; waits for the TV's refresh */
}

/* ---------------------------------------------------------------- textures and meshes */

struct PlatTexture
{
    int gpu; /* the GPU interface's texture id, -1 until first drawn */
    int width, height;
    uint8_t *rgba; /* the pixels, kept: the GPU layer borrows them */
    bool smooth, mips;
};

static int texture_gpu(PlatTexture *t)
{
    if (t->gpu < 0)
        t->gpu = vkp_ui_texture_create(t->rgba, t->width, t->height, t->smooth, t->mips, false);
    return t->gpu;
}

static PlatTexture *texture_create(const uint8_t *rgba, int width, int height, bool smooth, bool mips)
{
    PlatTexture *t = (PlatTexture *)calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->gpu = -1;
    t->rgba = (uint8_t *)malloc((size_t)width * height * 4);
    if (!t->rgba)
    {
        free(t);
        return NULL;
    }
    memcpy(t->rgba, rgba, (size_t)width * height * 4);
    t->width = width;
    t->height = height;
    t->smooth = smooth;
    t->mips = mips;
    return t;
}

extern "C" PlatTexture *plat_texture_create(const uint8_t *rgba, int width, int height, bool smooth)
{
    return texture_create(rgba, width, height, smooth, false);
}

extern "C" PlatTexture *plat_texture_create_mips(const uint8_t *rgba, int width, int height)
{
    return texture_create(rgba, width, height, true, true);
}

extern "C" void plat_texture_free(PlatTexture *t)
{
    if (!t)
        return;
    if (t->gpu >= 0)
        vkp_ui_texture_free(t->gpu);
    free(t->rgba);
    free(t);
}

extern "C" void plat_texture_size(const PlatTexture *t, int *width, int *height)
{
    *width = t ? t->width : 0;
    *height = t ? t->height : 0;
}

extern "C" void plat_draw_mesh(PlatTexture *texture, const PlatVertex *v, int count, const int *indices, int index_count)
{
    /* PlatVertex and VkpUiVertex are the same four floats and a colour */
    const int tex = texture ? texture_gpu(texture) : -1;
    if (texture && tex < 0)
        return;
    if (!indices)
    {
        vkp_ui_draw(tex, (const VkpUiVertex *)v, count - count % 3);
        return;
    }
    VkpUiVertex out[1536];
    for (int i = 0; i + 2 < index_count;)
    {
        int n = 0;
        for (; i + 2 < index_count && n + 3 <= 1536; i += 3, n += 3)
        {
            out[n] = *(const VkpUiVertex *)&v[indices[i]];
            out[n + 1] = *(const VkpUiVertex *)&v[indices[i + 1]];
            out[n + 2] = *(const VkpUiVertex *)&v[indices[i + 2]];
        }
        vkp_ui_draw(tex, out, n);
    }
}

extern "C" void plat_draw_texture(PlatTexture *texture, float x, float y, float w, float h, uint32_t tint, bool blend)
{
    if (!texture)
        return;
    const int id = texture_gpu(texture);
    if (id >= 0 && (tint >> 24) != 0)
    {
        int x0 = (int)(x + 0.5f), y0 = (int)(y + 0.5f);
        vkp_ui_blend(blend); /* a picture drawn without blending ignores its alpha */
        gpu_quad(id, (float)x0, (float)y0, (float)(x0 + (int)(w + 0.5f)), (float)(y0 + (int)(h + 0.5f)), 0, 0, 1, 1, tint);
        vkp_ui_blend(true);
    }
}

extern "C" void plat_draw_texture_region(PlatTexture *texture, int sx, int sy, int sw, int sh, float x, float y, float w,
                                         float h, uint32_t tint)
{
    if (!texture || (tint >> 24) == 0)
        return;
    const int id = texture_gpu(texture);
    int gx0 = (int)(x + 0.5f), gy0 = (int)(y + 0.5f);
    int gw = (int)(x + w + 0.5f) - gx0, gh = (int)(y + h + 0.5f) - gy0;
    if (id >= 0 && gw > 0 && gh > 0)
        gpu_quad(id, (float)gx0, (float)gy0, (float)(gx0 + gw), (float)(gy0 + gh), (float)sx / texture->width,
                 (float)sy / texture->height, (float)(sx + sw) / texture->width, (float)(sy + sh) / texture->height, tint);
}

extern "C" void plat_fill_rectf(float x, float y, float w, float h, uint32_t argb)
{
    if ((argb >> 24) == 0)
        return;
    int x0 = (int)(x + 0.5f), y0 = (int)(y + 0.5f);
    int rw = (int)(x + w + 0.5f) - x0, rh = (int)(y + h + 0.5f) - y0;
    if (rw > 0 && rh > 0)
        gpu_quad(-1, (float)x0, (float)y0, (float)(x0 + rw), (float)(y0 + rh), 0, 0, 0, 0, argb);
}

extern "C" void plat_set_clip(int x, int y, int w, int h) { vkp_ui_clip(x, y, w, h); }

/* ---------------------------------------------------------------- the system */

extern "C" void plat_asset_path(char *out, size_t size, const char *relative)
{
    char base[SwanStationPS5_PATH_MAX];
    path_join(base, sizeof(base), uwp_install_folder(), "assets");
    path_join(out, size, base, relative);
}

extern "C" uint64_t plat_ticks_us(void)
{
    static LARGE_INTEGER freq;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)((double)now.QuadPart * 1000000.0 / (double)freq.QuadPart);
}

extern "C" void plat_sleep_us(uint32_t us)
{
    Sleep(us / 1000);
}

extern "C" void plat_notify(const char *message)
{
    SwanStationPS5_log("notify: %s", message);
}

extern "C" void plat_clock(char *out, size_t size)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t format[80] = L"";
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STIMEFORMAT, format, 80);
    const bool h24 = wcschr(format, L'H') != NULL;
    if (h24)
        snprintf(out, size, "%02d:%02d", t.wHour, t.wMinute);
    else
        snprintf(out, size, "%d:%02d %s", t.wHour % 12 ? t.wHour % 12 : 12, t.wMinute, t.wHour < 12 ? "AM" : "PM");
}

extern "C" int plat_system_language(void)
{
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = L"";
    GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH);
    if (!wcsncmp(name, L"fr", 2)) return LANG_FR;
    if (!wcsncmp(name, L"pt", 2)) return LANG_PT;
    if (!wcsncmp(name, L"es", 2)) return LANG_ES;
    if (!wcsncmp(name, L"ja", 2)) return LANG_JA;
    return LANG_EN;
}
