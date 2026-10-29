/*
 * SwanStationUWP - the multi-pass shader chains (CRT Royale, NTSC, FSR 1, the other CRT presets) on Direct3D 11.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A chain is a list of passes (shaders_royale.h, shaders_ntsc.h, shaders_crt.h: DXBC made from the Vulkan renderer's SPIR-V
 * by tools/make-d3d-chains.py), each drawing a full-screen strip into an image the next one reads. The first pass reads the
 * game's picture; the last one is drawn into whatever target the caller has bound, inside a rectangle.
 */
#ifndef SwanStationUWP_DX_CHAIN_H
#define SwanStationUWP_DX_CHAIN_H

#include <d3d11.h>
#include <stdbool.h>

/* the chains: 0 CRT Royale Fast, 1 to 4 the NTSC presets, 5 FSR 1, 6 to 9 the presets of shaders_crt.h */
#define DXC_FSR 5

struct DxChain;
DxChain *dxc_create(ID3D11Device *device, ID3D11DeviceContext *context);
void dxc_destroy(DxChain *chain);

/* Sizes the chain for a PS1 picture of native_w x native_h shown in a viewport_w x viewport_h rectangle (the images are made
 * again when they change). offscreen: the last pass draws into an image too (FSR 1). The picture it reads is source_w x
 * source_h. False when the chain cannot be set up. */
bool dxc_prepare(DxChain *chain, int preset, bool offscreen, int native_w, int native_h, int viewport_w, int viewport_h,
                 int source_w, int source_h);
/* Draws every pass but the last (all of them when offscreen), reading source. Leaves the render target unbound. */
void dxc_record(DxChain *chain, ID3D11ShaderResourceView *source, bool source_is_scaled);
/* The last pass, into the target the caller has bound: the rectangle x, y, w, h of a target_w x target_h target. */
void dxc_final(DxChain *chain, ID3D11ShaderResourceView *source, bool source_is_scaled, int x, int y, int w, int h,
               int target_w, int target_h);
/* An offscreen chain's result. */
ID3D11ShaderResourceView *dxc_output(DxChain *chain);

#endif
