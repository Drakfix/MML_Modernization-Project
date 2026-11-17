#pragma once
/* RetroArch shader presets through librashader ([video] shader=...), for the Direct3D 11 backend
 * (Windows) and the Vulkan backend (everywhere). One chain at a time: the backend in use owns it. */
#ifdef _WIN32
#include <d3d11.h>
#endif
#ifdef MML_VULKAN
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#endif

int rshader_active(void);

#ifdef _WIN32
int rshader_load(ID3D11Device *dev, const char *game_dir, const char *preset_path);
/* swap presets while running ("" = off); 0 if the new one failed (then off) */
int rshader_reload(ID3D11Device *dev, const char *game_dir, const char *preset_path);
/* draw src through the preset into dst's rectangle; 0 = draw it yourself this time */
int rshader_frame(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst,
                  float x, float y, unsigned w, unsigned h);
#endif

#ifdef MML_VULKAN
typedef struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;                  /* used while building (the caller holds its queue lock) */
    PFN_vkGetInstanceProcAddr gipa;
    unsigned frames_in_flight;
} rshader_vk_device;

typedef struct {
    VkImage img;
    VkFormat fmt;
    unsigned w, h;
} rshader_vk_image;

/* Load a preset ("" = off), replacing the chain there was. The GPU must be idle (the old chain's
   objects go at once) and nothing else may use the queue meanwhile. 0 if it failed (then off). */
int rshader_vk_load(const rshader_vk_device *d, const char *game_dir, const char *preset_path);
/* Record the preset's passes into c, outside a render pass: src in SHADER_READ_ONLY_OPTIMAL (and
   made with TRANSFER_SRC: history copies), dst in COLOR_ATTACHMENT_OPTIMAL (stays so; the whole of
   it is cleared, the picture goes in the x, y, w, h rectangle). 0 = nothing recorded this time. */
int rshader_vk_frame(VkCommandBuffer c, const rshader_vk_image *src, const rshader_vk_image *dst, float x, float y,
                     unsigned w, unsigned h);
#endif
