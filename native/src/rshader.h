#pragma once
#include <d3d11.h>

/* RetroArch shader presets through librashader.dll ([video] shader=...). */
int rshader_load(ID3D11Device *dev, const char *game_dir, const char *preset_path);
int rshader_active(void);
/* swap presets while running ("" = off); 0 if the new one failed (then off) */
int rshader_reload(ID3D11Device *dev, const char *game_dir, const char *preset_path);
/* draw src through the preset into dst's rectangle; 0 = draw it yourself this time */
int rshader_frame(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst,
                  float x, float y, unsigned w, unsigned h);
