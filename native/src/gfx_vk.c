/* Vulkan backend for gfx.h ([video] backend=vulkan; built when the Vulkan headers and glslc are
 * there, MML_VULKAN).
 *
 * The same picture as gfx_d3d11.c, which it follows step by step: the game's frame goes into an
 * offscreen scene target (two: the game's frame and the in-between one), consecutive draws with
 * the same state are batched, and composing blits the scene into the window keeping the game's
 * aspect, puts our UI over it and presents, from a timer thread when interpolating.
 *
 * Vulkan 1.1 with VK_KHR_swapchain, loaded at run time (no import library: the game starts where
 * there is no Vulkan). Two frames in flight, each with its command buffer, vertex ring and
 * staging ring; what a frame used is destroyed once its fence has passed. The draw constants are
 * push constants; samplers (set 0) and images (set 1) are separate descriptor sets, so a texture
 * has one set whatever the filter. Pipelines are made on first use per blend and depth state.
 * Not yet: the effects depth target (ReShade), exclusive fullscreen, choosing the display's GPU,
 * RetroArch presets. */
#ifdef MML_VULKAN
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_backend.h"
#include "gfx_ui.h"
#include "gfx_win.h"
#include "log.h"

static const uint32_t spv_game_vert[] =
#include "shaders/vk_game_vert.inc"
;
static const uint32_t spv_game_frag[] =
#include "shaders/vk_game_frag.inc"
;
static const uint32_t spv_blit_vert[] =
#include "shaders/vk_blit_vert.inc"
;
static const uint32_t spv_blit_frag[] =
#include "shaders/vk_blit_frag.inc"
;
static const uint32_t spv_ui_vert[] =
#include "shaders/vk_ui_vert.inc"
;
static const uint32_t spv_ui_frag[] =
#include "shaders/vk_ui_frag.inc"
;

/* ---------------------------------------------------------------- the API, loaded */

#define VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceLayerProperties)
#define VK_INSTANCE(X)                                                                                         \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                         \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties)                          \
    X(vkGetPhysicalDeviceFeatures) X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice)                     \
    X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties) X(vkCreateWin32SurfaceKHR)                    \
    X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define VK_DEVICE(X)                                                                                            \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkQueueSubmit)                    \
    X(vkCreateImage) X(vkDestroyImage) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateBuffer)             \
    X(vkDestroyBuffer) X(vkGetImageMemoryRequirements) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory)     \
    X(vkFreeMemory) X(vkBindImageMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory)                  \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer)                 \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines)     \
    X(vkDestroyPipeline) X(vkCreatePipelineCache) X(vkCreateDescriptorSetLayout) X(vkCreateDescriptorPool)      \
    X(vkAllocateDescriptorSets) X(vkFreeDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateSampler)            \
    X(vkDestroySampler) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer)              \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences)          \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass)                    \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdPushConstants)             \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw) X(vkCmdClearAttachments) X(vkCmdPipelineBarrier)        \
    X(vkCmdCopyBufferToImage) X(vkCmdResolveImage)

#define VK_DECL(name) static PFN_##name name;
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
VK_GLOBAL(VK_DECL)
VK_INSTANCE(VK_DECL)
VK_DEVICE(VK_DECL)

/* ---------------------------------------------------------------- state */

#define FRAMES 2
#define VB_BYTES (4u << 20)        /* per frame: the game's vertices, then the UI's */
#define STAGING_BYTES (8u << 20)   /* per frame: texture uploads (bigger ones get their own buffer) */
#define MAX_SWAP 8
#define MAX_PIPES 128
#define MAX_DEAD 4096
#define MAX_SETS 8192

struct gfx_tex {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    VkDescriptorSet set;
    int w, h;
};

typedef struct {   /* game.hlsl's cbuffer Draw / draw.glsl's push constants (64 bytes) */
    float app_size[2];
    float textured, flat;
    float fog_color[4];
    float fog_start, fog_end, fog_on, fog_from_w;
    float alpha_test, alpha_func, alpha_ref, pad;
} draw_pc;

typedef struct {   /* a buffer mapped for the CPU */
    VkBuffer buf;
    VkDeviceMemory mem;
    uint8_t *map;
    VkDeviceSize size, pos;
} host_buf;

enum { DEAD_IMAGE, DEAD_VIEW, DEAD_BUFFER, DEAD_MEMORY, DEAD_SET };
typedef struct {
    int kind;
    uint64_t h;
} dead_obj;

typedef struct {
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acquired;
    host_buf vb, staging;
    dead_obj dead[MAX_DEAD];
    int ndead;
    int recording, submitted;
} frame;

typedef struct {
    VkImage color, resolved, depth;
    VkDeviceMemory color_mem, resolved_mem, depth_mem;
    VkImageView color_view, resolved_view, depth_view;
    VkFramebuffer fb;
    VkDescriptorSet set;   /* the image composing samples: resolved with MSAA, else color */
    int fresh;             /* not drawn into since made: the first pass clears */
} target;

static struct {
    int ready;
    HWND hwnd;
    gfx_options opt;
    int app_w, app_h;
    HMODULE lib;
    VkInstance inst;
    VkPhysicalDevice phys;
    VkPhysicalDeviceMemoryProperties memprops;
    float max_aniso;
    VkDevice dev;
    uint32_t qfam;
    VkQueue queue;
    CRITICAL_SECTION qlock;   /* submit (game thread) and present (timer thread) share the queue */
    VkSurfaceKHR surface;
    VkSwapchainKHR swap;
    VkFormat swap_fmt;
    VkImage swap_img[MAX_SWAP];
    VkImageView swap_view[MAX_SWAP];
    VkFramebuffer swap_fb[MAX_SWAP];
    VkSemaphore swap_done[MAX_SWAP];   /* rendering into that image finished */
    uint32_t nswap, swap_w, swap_h;
    int swap_dirty;                    /* recreate before the next frame (size, vsync) */
    int back_w, back_h;
    VkFormat depth_fmt;
    VkSampleCountFlagBits samples;
    VkRenderPass pass_clear, pass_load, pass_swap;
    target tg[2];
    int cur, tg_used[2];
    int scene_w, scene_h;
    VkCommandPool pool;
    frame fr[FRAMES];
    int fi;                            /* the frame being recorded */
    int pass_open;                     /* the scene pass of tg[cur] is open */
    VkDescriptorSetLayout smp_layout, img_layout;   /* set 0: a sampler; set 1: an image */
    VkPipelineLayout layout;
    VkDescriptorPool dpool;
    VkPipelineCache pcache;
    VkShaderModule sh_game_v, sh_game_f, sh_blit_v, sh_blit_f, sh_ui_v, sh_ui_f;
    VkSampler smp_point, smp_linear, smp_blit_linear, smp_blit_point;
    VkDescriptorSet set_point, set_linear, set_blit_linear, set_blit_point;
    struct { uint32_t key; VkPipeline p; } pipes[MAX_PIPES];
    int npipes;
    VkPipeline pipe_blit, pipe_ui;
    gfx_tex *white;
    gfx_tex *ui_atlas;
    unsigned ui_gen;
    /* current state */
    gfx_state st;
    gfx_tex *tex;
    int cb_dirty, state_dirty;
    VkPipeline bound_pipe;
    draw_pc pc;
} v;

static void flush_batch(void);
static void vk_wait_flip(void);
static void vk_present_target(int t);
static void vk_compose_target(int t);

/* ---------------------------------------------------------------- memory, buffers, images */

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    uint32_t i;
    for (i = 0; i < v.memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (v.memprops.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static VkDeviceMemory alloc_mem(VkMemoryRequirements req, VkMemoryPropertyFlags want)
{
    VkMemoryAllocateInfo ai;
    VkDeviceMemory m = VK_NULL_HANDLE;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mem_type(req.memoryTypeBits, want);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(v.dev, &ai, NULL, &m) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return m;
}

static int host_buf_make(host_buf *b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bi;
    VkMemoryRequirements req;
    void *p = NULL;
    memset(b, 0, sizeof *b);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(v.dev, &bi, NULL, &b->buf) != VK_SUCCESS)
        return 0;
    vkGetBufferMemoryRequirements(v.dev, b->buf, &req);
    b->mem = alloc_mem(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!b->mem || vkBindBufferMemory(v.dev, b->buf, b->mem, 0) != VK_SUCCESS ||
        vkMapMemory(v.dev, b->mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS)
        return 0;
    b->map = (uint8_t *)p;
    b->size = size;
    return 1;
}

static int image_make(int w, int h, VkFormat fmt, VkImageUsageFlags usage, VkSampleCountFlagBits samples,
                      VkImageAspectFlags aspect, VkImage *img, VkDeviceMemory *mem, VkImageView *view)
{
    VkImageCreateInfo ii;
    VkImageViewCreateInfo vi;
    VkMemoryRequirements req;
    memset(&ii, 0, sizeof ii);
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent.width = (uint32_t)w;
    ii.extent.height = (uint32_t)h;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = samples;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(v.dev, &ii, NULL, img) != VK_SUCCESS)
        return 0;
    vkGetImageMemoryRequirements(v.dev, *img, &req);
    *mem = alloc_mem(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!*mem || vkBindImageMemory(v.dev, *img, *mem, 0) != VK_SUCCESS)
        return 0;
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = *img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    return vkCreateImageView(v.dev, &vi, NULL, view) == VK_SUCCESS;
}

/* destroyed once the frame being recorded has finished on the GPU */
static void dead_add(int kind, uint64_t h)
{
    frame *f = &v.fr[v.fi];
    if (!h)
        return;
    if (f->ndead < MAX_DEAD) {
        f->dead[f->ndead].kind = kind;
        f->dead[f->ndead].h = h;
        f->ndead++;
    } else {   /* (never in practice) */
        vkDeviceWaitIdle(v.dev);
        f->dead[0].kind = kind;
        f->dead[0].h = h;
        f->ndead = 1;
    }
}

static void dead_free(frame *f)
{
    int k;
    for (k = 0; k < f->ndead; k++) {
        uint64_t h = f->dead[k].h;
        switch (f->dead[k].kind) {
        case DEAD_IMAGE: vkDestroyImage(v.dev, (VkImage)h, NULL); break;
        case DEAD_VIEW: vkDestroyImageView(v.dev, (VkImageView)h, NULL); break;
        case DEAD_BUFFER: vkDestroyBuffer(v.dev, (VkBuffer)h, NULL); break;
        case DEAD_MEMORY: vkFreeMemory(v.dev, (VkDeviceMemory)h, NULL); break;
        case DEAD_SET: { VkDescriptorSet s = (VkDescriptorSet)h; vkFreeDescriptorSets(v.dev, v.dpool, 1, &s); break; }
        }
    }
    f->ndead = 0;
}

static VkDescriptorSet image_set(VkImageView view)
{
    VkDescriptorSetAllocateInfo ai;
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet w;
    VkDescriptorSet s = VK_NULL_HANDLE;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = v.dpool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &v.img_layout;
    if (vkAllocateDescriptorSets(v.dev, &ai, &s) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    memset(&ii, 0, sizeof ii);
    ii.imageView = view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    memset(&w, 0, sizeof w);
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = s;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(v.dev, 1, &w, 0, NULL);
    return s;
}

static VkDescriptorSet sampler_set(VkSampler smp)
{
    VkDescriptorSetAllocateInfo ai;
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet w;
    VkDescriptorSet s = VK_NULL_HANDLE;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = v.dpool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &v.smp_layout;
    if (vkAllocateDescriptorSets(v.dev, &ai, &s) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    memset(&ii, 0, sizeof ii);
    ii.sampler = smp;
    memset(&w, 0, sizeof w);
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = s;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(v.dev, 1, &w, 0, NULL);
    return s;
}

static VkSampler make_sampler(VkFilter f, VkSamplerAddressMode a, float aniso)
{
    VkSamplerCreateInfo si;
    VkSampler s = VK_NULL_HANDLE;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = f;
    si.mipmapMode = f == VK_FILTER_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = a;
    if (aniso > 1 && v.max_aniso > 1) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = aniso < v.max_aniso ? aniso : v.max_aniso;
    }
    si.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(v.dev, &si, NULL, &s);
    return s;
}

/* ---------------------------------------------------------------- commands */

static void barrier(VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                    VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(v.fr[v.fi].cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* the frame's command buffer, recording (waits for the GPU to finish what it last held) */
static VkCommandBuffer cmd(void)
{
    frame *f = &v.fr[v.fi];
    if (!f->recording) {
        VkCommandBufferBeginInfo bi;
        if (f->submitted) {
            vkWaitForFences(v.dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
            vkResetFences(v.dev, 1, &f->fence);
            f->submitted = 0;
        }
        dead_free(f);
        f->vb.pos = 0;
        f->staging.pos = 0;
        vkResetCommandBuffer(f->cmd, 0);
        memset(&bi, 0, sizeof bi);
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f->cmd, &bi);
        f->recording = 1;
        v.bound_pipe = VK_NULL_HANDLE;
    }
    return f->cmd;
}

static void end_pass(void)
{
    flush_batch();
    if (v.pass_open) {
        vkCmdEndRenderPass(v.fr[v.fi].cmd);
        v.pass_open = 0;
    }
}

static void set_viewport(float x, float y, float w, float h, int sw, int sh)
{
    VkViewport vp;
    VkRect2D sc;
    vp.x = x;
    vp.y = y;
    vp.width = w;
    vp.height = h;
    vp.minDepth = 0;
    vp.maxDepth = 1;
    sc.offset.x = sc.offset.y = 0;
    sc.extent.width = (uint32_t)sw;
    sc.extent.height = (uint32_t)sh;
    vkCmdSetViewport(v.fr[v.fi].cmd, 0, 1, &vp);
    vkCmdSetScissor(v.fr[v.fi].cmd, 0, 1, &sc);
}

/* the scene pass of the current target, open (bind_scene in gfx_d3d11.c) */
static void open_scene(void)
{
    target *t = &v.tg[v.cur];
    VkRenderPassBeginInfo rb;
    VkClearValue clr[2];
    VkCommandBuffer c = cmd();
    if (v.pass_open)
        return;
    memset(&rb, 0, sizeof rb);
    memset(clr, 0, sizeof clr);
    clr[0].color.float32[3] = 1.0f;
    clr[1].depthStencil.depth = 1.0f;
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = t->fresh ? v.pass_clear : v.pass_load;
    rb.framebuffer = t->fb;
    rb.renderArea.extent.width = (uint32_t)v.scene_w;
    rb.renderArea.extent.height = (uint32_t)v.scene_h;
    rb.clearValueCount = t->fresh ? 2 : 0;
    rb.pClearValues = clr;
    vkCmdBeginRenderPass(c, &rb, VK_SUBPASS_CONTENTS_INLINE);
    t->fresh = 0;
    v.pass_open = 1;
    set_viewport(0, 0, (float)v.scene_w, (float)v.scene_h, v.scene_w, v.scene_h);
    v.bound_pipe = VK_NULL_HANDLE;
    v.state_dirty = v.cb_dirty = 1;
}

/* ---------------------------------------------------------------- render passes and targets */

static VkRenderPass make_pass(VkFormat color, VkFormat depth, VkSampleCountFlagBits samples, int clear, int swap)
{
    VkAttachmentDescription at[2];
    VkAttachmentReference cr, dr;
    VkSubpassDescription sp;
    VkSubpassDependency dep[2];
    VkRenderPassCreateInfo ri;
    VkRenderPass p = VK_NULL_HANDLE;
    memset(at, 0, sizeof at);
    at[0].format = color;
    at[0].samples = samples;
    at[0].loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].initialLayout = clear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    at[0].finalLayout = swap ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    at[1].format = depth;
    at[1].samples = samples;
    at[1].loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    at[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[1].initialLayout = clear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    cr.attachment = 0;
    cr.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    dr.attachment = 1;
    dr.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    memset(&sp, 0, sizeof sp);
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &cr;
    sp.pDepthStencilAttachment = swap ? NULL : &dr;
    /* everything before (draws, copies, blits) is done with what the pass touches, and the
       pass is done before whatever comes after */
    memset(dep, 0, sizeof dep);
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    dep[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    dep[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[1].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ri.attachmentCount = swap ? 1 : 2;
    ri.pAttachments = at;
    ri.subpassCount = 1;
    ri.pSubpasses = &sp;
    ri.dependencyCount = 2;
    ri.pDependencies = dep;
    vkCreateRenderPass(v.dev, &ri, NULL, &p);
    return p;
}

static void scene_size(int *w, int *h)
{
    if (v.opt.render_scale > 0)
        *h = v.app_h * v.opt.render_scale;
    else
        *h = v.back_h > 0 ? v.back_h : v.app_h;
    *w = (int)((double)*h * v.app_w / v.app_h + 0.5);
    if (*w > 16384) *w = 16384;
    if (*h > 16384) *h = 16384;
}

static void release_target(target *t)
{
    dead_add(DEAD_SET, (uint64_t)t->set);
    if (t->fb)
        vkDestroyFramebuffer(v.dev, t->fb, NULL);   /* (callers have waited for the GPU) */
    dead_add(DEAD_VIEW, (uint64_t)t->color_view);
    dead_add(DEAD_VIEW, (uint64_t)t->resolved_view);
    dead_add(DEAD_VIEW, (uint64_t)t->depth_view);
    dead_add(DEAD_IMAGE, (uint64_t)t->color);
    dead_add(DEAD_IMAGE, (uint64_t)t->resolved);
    dead_add(DEAD_IMAGE, (uint64_t)t->depth);
    dead_add(DEAD_MEMORY, (uint64_t)t->color_mem);
    dead_add(DEAD_MEMORY, (uint64_t)t->resolved_mem);
    dead_add(DEAD_MEMORY, (uint64_t)t->depth_mem);
    memset(t, 0, sizeof *t);
}

static int make_target(int i)
{
    target *t = &v.tg[i];
    VkFramebufferCreateInfo fi;
    VkImageView att[2];
    int w, h, ms = v.samples != VK_SAMPLE_COUNT_1_BIT;
    scene_size(&w, &h);
    if (!image_make(w, h, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (ms ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : VK_IMAGE_USAGE_SAMPLED_BIT),
                    v.samples, VK_IMAGE_ASPECT_COLOR_BIT, &t->color, &t->color_mem, &t->color_view))
        return 0;
    if (ms && !image_make(w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &t->resolved, &t->resolved_mem,
                          &t->resolved_view))
        return 0;
    if (!image_make(w, h, v.depth_fmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, v.samples,
                    VK_IMAGE_ASPECT_DEPTH_BIT, &t->depth, &t->depth_mem, &t->depth_view))
        return 0;
    att[0] = t->color_view;
    att[1] = t->depth_view;
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass = v.pass_load;
    fi.attachmentCount = 2;
    fi.pAttachments = att;
    fi.width = (uint32_t)w;
    fi.height = (uint32_t)h;
    fi.layers = 1;
    if (vkCreateFramebuffer(v.dev, &fi, NULL, &t->fb) != VK_SUCCESS)
        return 0;
    t->set = image_set(ms ? t->resolved_view : t->color_view);
    t->fresh = 1;
    if (ms) {   /* composing samples it before anything is resolved into it on a fresh target */
        end_pass();
        cmd();
        barrier(t->resolved, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                0, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    v.scene_w = w;
    v.scene_h = h;
    if (i == 0)
        log_printf("gfx: scene %dx%d (%dx MSAA) for the game's %dx%d, window %dx%d", w, h, (int)v.samples, v.app_w,
                   v.app_h, v.back_w, v.back_h);
    return t->set != VK_NULL_HANDLE;
}

static int make_scene(void)
{
    int i, ok = 1;
    v.tg_used[0] = 1;
    for (i = 0; i < 2; i++)
        if (v.tg_used[i])
            ok &= make_target(i);
    return ok;
}

static void release_scene(void)
{
    int i;
    for (i = 0; i < 2; i++)
        release_target(&v.tg[i]);
}

/* ---------------------------------------------------------------- the swapchain */

static void release_swap(void)
{
    uint32_t i;
    for (i = 0; i < v.nswap; i++) {
        vkDestroyFramebuffer(v.dev, v.swap_fb[i], NULL);
        vkDestroyImageView(v.dev, v.swap_view[i], NULL);
        vkDestroySemaphore(v.dev, v.swap_done[i], NULL);
    }
    v.nswap = 0;
}

static VkPresentModeKHR present_mode(void)
{
    VkPresentModeKHR modes[16], want[3];
    uint32_t n = 0, i, k, nw = 0;
    if (v.opt.vsync)
        return VK_PRESENT_MODE_FIFO_KHR;
    /* DXGI's flip model with sync interval 0 shows the newest frame without tearing: mailbox */
    want[nw++] = VK_PRESENT_MODE_MAILBOX_KHR;
    want[nw++] = VK_PRESENT_MODE_IMMEDIATE_KHR;
    vkGetPhysicalDeviceSurfacePresentModesKHR(v.phys, v.surface, &n, NULL);
    if (n > 16)
        n = 16;
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(v.phys, v.surface, &n, modes) < 0)
        return VK_PRESENT_MODE_FIFO_KHR;
    for (k = 0; k < nw; k++)
        for (i = 0; i < n; i++)
            if (modes[i] == want[k])
                return want[k];
    return VK_PRESENT_MODE_FIFO_KHR;
}

static int make_swap(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSwapchainCreateInfoKHR si;
    VkSwapchainKHR old = v.swap;
    VkImageViewCreateInfo vi;
    VkFramebufferCreateInfo fi;
    VkSemaphoreCreateInfo semi;
    uint32_t n, i;
    int w, h;
    vkDeviceWaitIdle(v.dev);
    release_swap();
    gfx_win_client_size(v.hwnd, &w, &h);
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(v.phys, v.surface, &caps) != VK_SUCCESS)
        return 0;
    if (caps.currentExtent.width != UINT32_MAX) {   /* the window's size, as the surface has it */
        w = (int)caps.currentExtent.width;
        h = (int)caps.currentExtent.height;
    }
    if (w <= 0 || h <= 0)
        return 0;   /* minimised: try again later */
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    si.surface = v.surface;
    si.minImageCount = caps.minImageCount + 1 > 3 ? caps.minImageCount + 1 : 3;
    if (caps.maxImageCount && si.minImageCount > caps.maxImageCount)
        si.minImageCount = caps.maxImageCount;
    si.imageFormat = v.swap_fmt;
    si.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    si.imageExtent.width = (uint32_t)w;
    si.imageExtent.height = (uint32_t)h;
    si.imageArrayLayers = 1;
    si.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    si.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    si.preTransform = caps.currentTransform;
    si.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    si.presentMode = present_mode();
    si.clipped = VK_TRUE;
    si.oldSwapchain = old;
    if (vkCreateSwapchainKHR(v.dev, &si, NULL, &v.swap) != VK_SUCCESS) {
        v.swap = VK_NULL_HANDLE;
        if (old)
            vkDestroySwapchainKHR(v.dev, old, NULL);
        return 0;
    }
    if (old)
        vkDestroySwapchainKHR(v.dev, old, NULL);
    n = MAX_SWAP;
    vkGetSwapchainImagesKHR(v.dev, v.swap, &n, v.swap_img);
    memset(&semi, 0, sizeof semi);
    semi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (i = 0; i < n; i++) {
        memset(&vi, 0, sizeof vi);
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = v.swap_img[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = v.swap_fmt;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        vkCreateImageView(v.dev, &vi, NULL, &v.swap_view[i]);
        memset(&fi, 0, sizeof fi);
        fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = v.pass_swap;
        fi.attachmentCount = 1;
        fi.pAttachments = &v.swap_view[i];
        fi.width = (uint32_t)w;
        fi.height = (uint32_t)h;
        fi.layers = 1;
        vkCreateFramebuffer(v.dev, &fi, NULL, &v.swap_fb[i]);
        vkCreateSemaphore(v.dev, &semi, NULL, &v.swap_done[i]);
    }
    v.nswap = n;
    v.swap_w = (uint32_t)w;
    v.swap_h = (uint32_t)h;
    v.back_w = w;
    v.back_h = h;
    v.swap_dirty = 0;
    return 1;
}

/* ---------------------------------------------------------------- pipelines */

static VkShaderModule shader(const uint32_t *code, size_t bytes)
{
    VkShaderModuleCreateInfo si;
    VkShaderModule m = VK_NULL_HANDLE;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    si.codeSize = bytes;
    si.pCode = code;
    vkCreateShaderModule(v.dev, &si, NULL, &m);
    return m;
}

enum { P_GAME, P_BLIT, P_UI };

static VkPipeline make_pipe(int kind, const gfx_state *s, VkBlendFactor src, VkBlendFactor dst, VkBlendFactor src_a,
                            VkBlendFactor dst_a)
{
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb;
    VkVertexInputAttributeDescription va[4];
    VkPipelineVertexInputStateCreateInfo vin;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy;
    VkGraphicsPipelineCreateInfo pi;
    VkPipeline p = VK_NULL_HANDLE;
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[0].pName = st[1].pName = "main";
    st[0].module = kind == P_GAME ? v.sh_game_v : kind == P_BLIT ? v.sh_blit_v : v.sh_ui_v;
    st[1].module = kind == P_GAME ? v.sh_game_f : kind == P_BLIT ? v.sh_blit_f : v.sh_ui_f;
    memset(&vin, 0, sizeof vin);
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    memset(&vb, 0, sizeof vb);
    memset(va, 0, sizeof va);
    if (kind == P_GAME) {
        vb.stride = sizeof(gfx_vertex);
        va[0].location = 0, va[0].format = VK_FORMAT_R32G32B32A32_SFLOAT, va[0].offset = 0;
        va[1].location = 1, va[1].format = VK_FORMAT_R8G8B8A8_UNORM, va[1].offset = 16;
        va[2].location = 3, va[2].format = VK_FORMAT_R32G32_SFLOAT, va[2].offset = 24;   /* (specular unused) */
        vin.vertexBindingDescriptionCount = 1;
        vin.pVertexBindingDescriptions = &vb;
        vin.vertexAttributeDescriptionCount = 3;
        vin.pVertexAttributeDescriptions = va;
    } else if (kind == P_UI) {
        vb.stride = sizeof(gfx_ui_vert);
        va[0].location = 0, va[0].format = VK_FORMAT_R32G32_SFLOAT, va[0].offset = 0;
        va[1].location = 1, va[1].format = VK_FORMAT_R32G32_SFLOAT, va[1].offset = 8;
        va[2].location = 2, va[2].format = VK_FORMAT_R8G8B8A8_UNORM, va[2].offset = 16;
        vin.vertexBindingDescriptionCount = 1;
        vin.pVertexBindingDescriptions = &vb;
        vin.vertexAttributeDescriptionCount = 3;
        vin.pVertexAttributeDescriptions = va;
    }
    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = vp.scissorCount = 1;
    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = kind == P_GAME ? v.samples : VK_SAMPLE_COUNT_1_BIT;
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    if (kind == P_GAME) {
        ds.depthTestEnable = s->z_enable ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = s->z_enable && s->z_write ? VK_TRUE : VK_FALSE;
        /* D3DCMPFUNC 1..8 (NEVER..ALWAYS) is VkCompareOp 0..7 */
        ds.depthCompareOp = s->z_func >= 1 && s->z_func <= 8 ? (VkCompareOp)(s->z_func - 1) : VK_COMPARE_OP_LESS_OR_EQUAL;
    }
    memset(&cba, 0, sizeof cba);
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                         VK_COLOR_COMPONENT_A_BIT;
    if ((kind == P_GAME && s->blend) || kind == P_UI) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = src;
        cba.dstColorBlendFactor = dst;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = src_a;
        cba.dstAlphaBlendFactor = dst_a;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    memset(&cb, 0, sizeof cb);
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    memset(&dy, 0, sizeof dy);
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vin;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = v.layout;
    pi.renderPass = kind == P_GAME ? v.pass_load : v.pass_swap;
    if (vkCreateGraphicsPipelines(v.dev, v.pcache, 1, &pi, NULL, &p) != VK_SUCCESS)
        log_printf("gfx: Vulkan pipeline failed (kind %d)", kind);
    return p;
}

/* D3DBLEND -> VkBlendFactor (the colour factor, or the alpha one), as gfx_d3d11.c's map_blend */
static VkBlendFactor map_blend(int b, int alpha)
{
    switch (b) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
    case 10: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

static VkPipeline game_pipe(void)
{
    const gfx_state *s = &v.st;
    uint32_t key = (s->blend ? (1u << 24) | ((uint32_t)s->src_blend << 16) | ((uint32_t)s->dst_blend << 8) : 0) |
                   ((uint32_t)!!s->z_enable << 5) | ((uint32_t)!!s->z_write << 4) | (s->z_func & 0xFu);
    VkPipeline p;
    int i;
    for (i = 0; i < v.npipes; i++)
        if (v.pipes[i].key == key)
            return v.pipes[i].p;
    p = make_pipe(P_GAME, s, map_blend(s->src_blend, 0), map_blend(s->dst_blend, 0), map_blend(s->src_blend, 1),
                  map_blend(s->dst_blend, 1));
    if (p && v.npipes < MAX_PIPES) {
        v.pipes[v.npipes].key = key;
        v.pipes[v.npipes].p = p;
        v.npipes++;
    }
    return p;
}

static void release_game_pipes(void)
{
    int i;
    for (i = 0; i < v.npipes; i++)
        vkDestroyPipeline(v.dev, v.pipes[i].p, NULL);
    v.npipes = 0;
}

/* ---------------------------------------------------------------- setup */

static int load_api(void)
{
    v.lib = LoadLibraryA("vulkan-1.dll");
    if (!v.lib) {
        log_printf("gfx: no Vulkan here (vulkan-1.dll)");
        return 0;
    }
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(v.lib, "vkGetInstanceProcAddr");
    if (!vkGetInstanceProcAddr)
        return 0;
#define VK_LOAD_GLOBAL(name) name = (PFN_##name)vkGetInstanceProcAddr(NULL, #name);
    VK_GLOBAL(VK_LOAD_GLOBAL)
    return vkCreateInstance != NULL;
}

static int has_layer(const char *name)
{
    VkLayerProperties lp[64];
    uint32_t n = 64, i;
    if (vkEnumerateInstanceLayerProperties(&n, lp) < 0)
        return 0;
    for (i = 0; i < n; i++)
        if (strcmp(lp[i].layerName, name) == 0)
            return 1;
    return 0;
}

/* MML_VK_VALIDATION=1 with the Vulkan SDK installed: the Khronos validation layer checks every
   call; each distinct warning or error goes to the log (the first few of each kind) */
static VKAPI_ATTR VkBool32 VKAPI_CALL on_validation(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                                     VkDebugUtilsMessageTypeFlagsEXT type,
                                                     const VkDebugUtilsMessengerCallbackDataEXT *d, void *user)
{
    static struct { int32_t id; int n; } seen[256];
    static int nseen, total;
    int k;
    (void)type; (void)user;
    if (!(sev & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)))
        return VK_FALSE;
    for (k = 0; k < nseen && seen[k].id != d->messageIdNumber; k++)
        ;
    if (k == nseen && nseen < 256)
        seen[nseen].id = d->messageIdNumber, seen[nseen].n = 0, nseen++;
    if (k < 256 && seen[k].n++ < 3 && total++ < 300)
        log_printf("vk %s: %s", sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "ERROR" : "warning",
                   d->pMessage ? d->pMessage : "?");
    return VK_FALSE;
}

static int make_instance(void)
{
    static const char *const ext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
                                       VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
    static const char *const layer = "VK_LAYER_KHRONOS_validation";
    VkApplicationInfo ai;
    VkInstanceCreateInfo ii;
    int validate = GetEnvironmentVariableA("MML_VK_VALIDATION", NULL, 0) > 0 && has_layer(layer);
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "Mega Man Legends";
    ai.pEngineName = "mml";
    ai.apiVersion = VK_API_VERSION_1_1;
    memset(&ii, 0, sizeof ii);
    ii.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ii.pApplicationInfo = &ai;
    ii.enabledExtensionCount = 2;
    ii.ppEnabledExtensionNames = ext;
    if (validate) {
        ii.enabledLayerCount = 1;
        ii.ppEnabledLayerNames = &layer;
        ii.enabledExtensionCount = 3;   /* + debug utils, for the messages */
        log_printf("gfx: Vulkan validation layer on");
    }
    if (vkCreateInstance(&ii, NULL, &v.inst) != VK_SUCCESS) {
        log_printf("gfx: vkCreateInstance failed");
        return 0;
    }
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT make =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(v.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi;
        VkDebugUtilsMessengerEXT m;
        memset(&mi, 0, sizeof mi);
        mi.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = on_validation;
        if (make)
            make(v.inst, &mi, NULL, &m);
    }
#define VK_LOAD_INSTANCE(name) name = (PFN_##name)vkGetInstanceProcAddr(v.inst, #name);
    VK_INSTANCE(VK_LOAD_INSTANCE)
    return vkCreateDevice != NULL && vkCreateWin32SurfaceKHR != NULL;
}

/* a GPU that can draw and present to the window, discrete preferred */
static int pick_device(void)
{
    VkPhysicalDevice devs[16];
    uint32_t n = 16, i, best_score = 0;
    if (vkEnumeratePhysicalDevices(v.inst, &n, devs) < 0 || !n)
        return 0;
    for (i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        VkQueueFamilyProperties qf[16];
        uint32_t nq = 0, q, score;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, NULL);
        if (nq > 16)
            nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, qf);
        for (q = 0; q < nq; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, v.surface, &present);
            if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
                break;
        }
        if (q == nq || p.apiVersion < VK_API_VERSION_1_1)
            continue;
        score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
              : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (score > best_score) {
            best_score = score;
            v.phys = devs[i];
            v.qfam = q;
        }
    }
    if (!v.phys)
        return 0;
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(v.phys, &p);
        v.max_aniso = p.limits.maxSamplerAnisotropy;
        log_printf("gfx: Vulkan device %s (API %u.%u.%u, driver %08X)", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
                   VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion);
    }
    vkGetPhysicalDeviceMemoryProperties(v.phys, &v.memprops);
    return 1;
}

static int make_device(void)
{
    static const char *const ext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi;
    VkDeviceCreateInfo di;
    VkPhysicalDeviceFeatures have, use;
    memset(&qi, 0, sizeof qi);
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = v.qfam;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    vkGetPhysicalDeviceFeatures(v.phys, &have);
    memset(&use, 0, sizeof use);
    use.samplerAnisotropy = have.samplerAnisotropy;
    if (!have.samplerAnisotropy)
        v.max_aniso = 1;
    memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = ext;
    di.pEnabledFeatures = &use;
    if (vkCreateDevice(v.phys, &di, NULL, &v.dev) != VK_SUCCESS) {
        log_printf("gfx: vkCreateDevice failed");
        return 0;
    }
#define VK_LOAD_DEVICE(name) name = (PFN_##name)vkGetDeviceProcAddr(v.dev, #name);
    VK_DEVICE(VK_LOAD_DEVICE)
    vkGetDeviceQueue(v.dev, v.qfam, 0, &v.queue);
    InitializeCriticalSection(&v.qlock);
    return 1;
}

static VkFormat pick_swap_format(void)
{
    VkSurfaceFormatKHR f[64];
    uint32_t n = 0, i;
    vkGetPhysicalDeviceSurfaceFormatsKHR(v.phys, v.surface, &n, NULL);
    if (n > 64)
        n = 64;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(v.phys, v.surface, &n, f) < 0 || !n)
        return VK_FORMAT_B8G8R8A8_UNORM;
    for (i = 0; i < n; i++)   /* plain 8-bit UNORM, as the D3D11 swap chain (no sRGB conversion) */
        if (f[i].format == VK_FORMAT_B8G8R8A8_UNORM || f[i].format == VK_FORMAT_R8G8B8A8_UNORM)
            return f[i].format;
    return f[0].format;
}

static VkFormat pick_depth_format(void)
{
    /* D24 as the D3D11 backend (same depth precision for the game's comparisons), else D32F */
    static const VkFormat fmts[] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM };
    size_t i;
    for (i = 0; i < sizeof fmts / sizeof fmts[0]; i++) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(v.phys, fmts[i], &p);
        if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return fmts[i];
    }
    return VK_FORMAT_D32_SFLOAT;
}

static VkSampleCountFlagBits pick_samples(int msaa)
{
    VkPhysicalDeviceProperties p;
    VkSampleCountFlags ok;
    int s;
    vkGetPhysicalDeviceProperties(v.phys, &p);
    ok = p.limits.framebufferColorSampleCounts & p.limits.framebufferDepthSampleCounts;
    for (s = msaa > 8 ? 8 : msaa; s > 1; s >>= 1)
        if (ok & (VkSampleCountFlags)s)
            return (VkSampleCountFlagBits)s;
    if (msaa > 1)
        log_printf("gfx: %dx MSAA not supported, off", msaa);
    return VK_SAMPLE_COUNT_1_BIT;
}

static int make_frames(void)
{
    VkCommandPoolCreateInfo pi;
    VkCommandBufferAllocateInfo ai;
    VkFenceCreateInfo fi;
    VkSemaphoreCreateInfo si;
    int i;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = v.qfam;
    if (vkCreateCommandPool(v.dev, &pi, NULL, &v.pool) != VK_SUCCESS)
        return 0;
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (i = 0; i < FRAMES; i++) {
        frame *f = &v.fr[i];
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = v.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(v.dev, &ai, &f->cmd) != VK_SUCCESS ||
            vkCreateFence(v.dev, &fi, NULL, &f->fence) != VK_SUCCESS ||
            vkCreateSemaphore(v.dev, &si, NULL, &f->acquired) != VK_SUCCESS ||
            !host_buf_make(&f->vb, VB_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
            !host_buf_make(&f->staging, STAGING_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
            return 0;
    }
    return 1;
}

static int make_layouts(void)
{
    VkDescriptorSetLayoutBinding b[2];
    VkDescriptorSetLayoutCreateInfo li;
    VkPushConstantRange pr;
    VkPipelineLayoutCreateInfo pi;
    VkDescriptorPoolSize ps[2];
    VkDescriptorPoolCreateInfo dpi;
    VkPipelineCacheCreateInfo ci;
    VkDescriptorSetLayout sets[2];
    /* set 0 holds a sampler, set 1 an image: a texture's set takes one image descriptor, and the
       filter (point / linear / anisotropic) is a separate set to bind */
    memset(b, 0, sizeof b);
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1] = b[0];
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    memset(&li, 0, sizeof li);
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 1;
    li.pBindings = &b[0];
    if (vkCreateDescriptorSetLayout(v.dev, &li, NULL, &v.smp_layout) != VK_SUCCESS)
        return 0;
    li.pBindings = &b[1];
    if (vkCreateDescriptorSetLayout(v.dev, &li, NULL, &v.img_layout) != VK_SUCCESS)
        return 0;
    sets[0] = v.smp_layout;
    sets[1] = v.img_layout;
    memset(&pr, 0, sizeof pr);
    pr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pr.size = sizeof(draw_pc);
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pi.setLayoutCount = 2;
    pi.pSetLayouts = sets;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(v.dev, &pi, NULL, &v.layout) != VK_SUCCESS)
        return 0;
    ps[0].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    ps[0].descriptorCount = 16;
    ps[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    ps[1].descriptorCount = MAX_SETS;
    memset(&dpi, 0, sizeof dpi);
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = MAX_SETS + 16;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(v.dev, &dpi, NULL, &v.dpool) != VK_SUCCESS)
        return 0;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    vkCreatePipelineCache(v.dev, &ci, NULL, &v.pcache);
    return 1;
}

static void make_samplers(void)
{
    v.smp_point = make_sampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT, 0);
    v.smp_linear = make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, (float)v.opt.anisotropy);
    v.smp_blit_linear = make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0);
    v.smp_blit_point = make_sampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0);
    v.set_point = sampler_set(v.smp_point);
    v.set_linear = sampler_set(v.smp_linear);
    v.set_blit_linear = sampler_set(v.smp_blit_linear);
    v.set_blit_point = sampler_set(v.smp_blit_point);
}

static void vk_prepare_window(void *window, int app_w, int app_h, const gfx_options *opt)
{
    if (v.ready || !window)
        return;
    v.hwnd = (HWND)window;
    v.opt = *opt;
    v.app_w = app_w > 0 ? app_w : 640;
    v.app_h = app_h > 0 ? app_h : 480;
    gfx_win_place(v.hwnd, &v.opt, v.app_w, v.app_h);
    gfx_win_fill_black(v.hwnd);
}

static gfx_tex *vk_tex_create(int w, int h);
static void vk_tex_upload(gfx_tex *t, const uint32_t *rgba, int w, int h);
static void submit_frame(int with_image);

static int vk_init(void *window, int app_w, int app_h, const gfx_options *opt)
{
    VkWin32SurfaceCreateInfoKHR si;
    if (v.ready)
        return 1;
    v.hwnd = (HWND)window;
    v.opt = *opt;
    v.app_w = app_w;
    v.app_h = app_h;
    if (!load_api() || !make_instance())
        return 0;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    si.hinstance = GetModuleHandleA(NULL);
    si.hwnd = v.hwnd;
    if (vkCreateWin32SurfaceKHR(v.inst, &si, NULL, &v.surface) != VK_SUCCESS) {
        log_printf("gfx: Vulkan surface failed");
        return 0;
    }
    if (!pick_device()) {
        log_printf("gfx: no Vulkan 1.1 GPU that can show the window");
        return 0;
    }
    if (!make_device())
        return 0;
    gfx_win_place(v.hwnd, &v.opt, v.app_w, v.app_h);
    v.swap_fmt = pick_swap_format();
    v.depth_fmt = pick_depth_format();
    v.samples = pick_samples(v.opt.msaa);
    v.pass_clear = make_pass(VK_FORMAT_R8G8B8A8_UNORM, v.depth_fmt, v.samples, 1, 0);
    v.pass_load = make_pass(VK_FORMAT_R8G8B8A8_UNORM, v.depth_fmt, v.samples, 0, 0);
    v.pass_swap = make_pass(v.swap_fmt, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, 1, 1);
    if (!v.pass_clear || !v.pass_load || !v.pass_swap || !make_frames() || !make_layouts())
        return 0;
    v.sh_game_v = shader(spv_game_vert, sizeof spv_game_vert);
    v.sh_game_f = shader(spv_game_frag, sizeof spv_game_frag);
    v.sh_blit_v = shader(spv_blit_vert, sizeof spv_blit_vert);
    v.sh_blit_f = shader(spv_blit_frag, sizeof spv_blit_frag);
    v.sh_ui_v = shader(spv_ui_vert, sizeof spv_ui_vert);
    v.sh_ui_f = shader(spv_ui_frag, sizeof spv_ui_frag);
    make_samplers();
    if (!make_swap() || !make_scene())
        return 0;
    v.pipe_blit = make_pipe(P_BLIT, &v.st, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE,
                            VK_BLEND_FACTOR_ZERO);
    v.pipe_ui = make_pipe(P_UI, &v.st, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                          VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    if (!v.sh_game_v || !v.sh_game_f || !v.pipe_blit || !v.pipe_ui) {
        log_printf("gfx: Vulkan pipeline setup failed");
        return 0;
    }
    v.ready = 1;
    {   /* untextured draws still bind an image */
        static const uint32_t white_px = 0xFFFFFFFFu;
        v.white = vk_tex_create(1, 1);
        vk_tex_upload(v.white, &white_px, 1, 1);
    }
    v.cb_dirty = v.state_dirty = 1;
    if (v.opt.shader[0])
        log_printf("gfx: RetroArch presets aren't in the Vulkan backend yet");
    log_printf("gfx: Vulkan ready (%s)", v.opt.windowed ? "windowed" : "borderless fullscreen");
    return 1;
}

static int vk_ready(void)
{
    return v.ready;
}

static void vk_set_mode(int app_w, int app_h)
{
    if (app_w <= 0 || app_h <= 0 || (app_w == v.app_w && app_h == v.app_h))
        return;
    v.app_w = app_w;
    v.app_h = app_h;
    if (v.ready) {
        end_pass();
        submit_frame(0);
        vk_wait_flip();
        vkDeviceWaitIdle(v.dev);
        release_scene();
        make_scene();
        v.cb_dirty = 1;
    }
}

/* ---------------------------------------------------------------- textures */

static gfx_tex *vk_tex_create_fmt(int w, int h, VkFormat fmt)
{
    gfx_tex *t;
    if (!v.ready)
        return NULL;
    t = (gfx_tex *)calloc(1, sizeof *t);
    if (!t)
        return NULL;
    if (!image_make(w, h, fmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &t->img, &t->mem, &t->view) ||
        !(t->set = image_set(t->view))) {
        if (t->view) vkDestroyImageView(v.dev, t->view, NULL);
        if (t->img) vkDestroyImage(v.dev, t->img, NULL);
        if (t->mem) vkFreeMemory(v.dev, t->mem, NULL);
        free(t);
        return NULL;
    }
    t->w = w;
    t->h = h;
    /* ready to be sampled from now on (its contents until the first upload don't matter) */
    end_pass();
    cmd();
    barrier(t->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    return t;
}

static gfx_tex *vk_tex_create(int w, int h)
{
    return vk_tex_create_fmt(w, h, VK_FORMAT_R8G8B8A8_UNORM);
}

/* pixels (bpp bytes each, tightly packed) into the image, in command order with the draws */
static void upload(gfx_tex *t, const void *px, int bpp)
{
    frame *f = &v.fr[v.fi];
    VkDeviceSize bytes = (VkDeviceSize)t->w * t->h * bpp, at;
    VkBuffer src;
    VkBufferImageCopy r;
    VkCommandBuffer c;
    host_buf own;
    end_pass();
    c = cmd();
    at = (f->staging.pos + 15) & ~(VkDeviceSize)15;
    if (at + bytes <= f->staging.size) {
        memcpy(f->staging.map + at, px, (size_t)bytes);
        f->staging.pos = at + bytes;
        src = f->staging.buf;
    } else {   /* bigger than what's left: a buffer of its own, freed with the frame */
        if (!host_buf_make(&own, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
            return;
        memcpy(own.map, px, (size_t)bytes);
        dead_add(DEAD_BUFFER, (uint64_t)own.buf);
        dead_add(DEAD_MEMORY, (uint64_t)own.mem);
        src = own.buf;
        at = 0;
    }
    barrier(t->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    memset(&r, 0, sizeof r);
    r.bufferOffset = at;
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = (uint32_t)t->w;
    r.imageExtent.height = (uint32_t)t->h;
    r.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(c, src, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    barrier(t->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

static void vk_tex_upload(gfx_tex *t, const uint32_t *rgba, int w, int h)
{
    if (!t || !v.ready || w != t->w || h != t->h)
        return;
    upload(t, rgba, 4);   /* (after the queued draws, which see the old contents) */
}

static void vk_tex_size(const gfx_tex *t, int *w, int *h)
{
    *w = t ? t->w : 0;
    *h = t ? t->h : 0;
}

static void vk_tex_destroy(gfx_tex *t)
{
    if (!t)
        return;
    flush_batch();
    if (v.tex == t)
        v.tex = NULL;
    dead_add(DEAD_SET, (uint64_t)t->set);
    dead_add(DEAD_VIEW, (uint64_t)t->view);
    dead_add(DEAD_IMAGE, (uint64_t)t->img);
    dead_add(DEAD_MEMORY, (uint64_t)t->mem);
    free(t);
}

static void vk_set_texture(gfx_tex *t)
{
    if (v.tex != t) {
        v.tex = t;
        v.cb_dirty = 1;
    }
}

static void vk_set_state(const gfx_state *s)
{
    if (memcmp(&v.st, s, sizeof *s) != 0) {
        v.st = *s;
        v.state_dirty = v.cb_dirty = 1;
    }
}

/* ---------------------------------------------------------------- drawing */

static void apply_state(void)
{
    VkCommandBuffer c = v.fr[v.fi].cmd;
    if (v.state_dirty) {
        VkPipeline p = game_pipe();
        VkDescriptorSet s = v.st.linear ? v.set_linear : v.set_point;
        if (p != v.bound_pipe) {
            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
            v.bound_pipe = p;
        }
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, v.layout, 0, 1, &s, 0, NULL);
        v.state_dirty = 0;
    }
    if (v.cb_dirty) {
        draw_pc *d = &v.pc;
        gfx_tex *t = v.tex ? v.tex : v.white;
        d->app_size[0] = (float)v.app_w;
        d->app_size[1] = (float)v.app_h;
        d->textured = v.tex ? 1.0f : 0.0f;
        d->flat = v.st.flat ? 1.0f : 0.0f;
        d->fog_color[0] = ((v.st.fog_color >> 16) & 0xFF) / 255.0f;
        d->fog_color[1] = ((v.st.fog_color >> 8) & 0xFF) / 255.0f;
        d->fog_color[2] = (v.st.fog_color & 0xFF) / 255.0f;
        d->fog_color[3] = 1.0f;
        d->fog_start = v.st.fog_start;
        d->fog_end = v.st.fog_end;
        d->fog_on = (v.st.fog && !v.opt.no_fog) ? 1.0f : 0.0f;
        d->fog_from_w = v.opt.fog_from_w ? 1.0f : 0.0f;
        d->alpha_test = v.st.alpha_test ? 1.0f : 0.0f;
        d->alpha_func = (float)v.st.alpha_func;
        d->alpha_ref = v.st.alpha_ref / 255.0f;
        vkCmdPushConstants(c, v.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof *d, d);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, v.layout, 1, 1, &t->set, 0, NULL);
        v.cb_dirty = 0;
    }
}

/* Batching as in gfx_d3d11.c: consecutive draws with the same state and texture go out as one
   triangle list when anything changes. */
#define BATCH_MAX (3 * 16384)
static gfx_vertex g_batch[BATCH_MAX];
static int g_nbatch;

static void flush_batch(void)
{
    frame *f = &v.fr[v.fi];
    VkDeviceSize bytes, at;
    int n = g_nbatch;
    if (!n)
        return;
    g_nbatch = 0;
    bytes = (VkDeviceSize)n * sizeof(gfx_vertex);
    at = f->vb.pos;
    if (at + bytes > f->vb.size)
        return;   /* (4 MB of the game's vertices in one frame: never) */
    memcpy(f->vb.map + at, g_batch, (size_t)bytes);
    f->vb.pos = at + bytes;
    vkCmdBindVertexBuffers(f->cmd, 0, 1, &f->vb.buf, &at);
    vkCmdDraw(f->cmd, (uint32_t)n, 1, 0, 0);
}

static void vk_draw(int prim, const gfx_vertex *vtx, int count)
{
    int need, i;
    gfx_vertex *o;
    if (!v.ready || count < 3)
        return;
    if (!v.pass_open) {
        open_scene();
    }
    if (v.state_dirty || v.cb_dirty) {
        flush_batch();   /* what's queued goes out with the state it was made with */
        apply_state();
    }
    need = prim == GFX_TRILIST ? count - count % 3 : (count - 2) * 3;
    if (need > BATCH_MAX)
        return;
    if (g_nbatch + need > BATCH_MAX)
        flush_batch();
    o = g_batch + g_nbatch;
    if (prim == GFX_TRILIST) {
        memcpy(o, vtx, sizeof *vtx * (size_t)need);
    } else if (prim == GFX_TRISTRIP) {
        for (i = 0; i + 2 < count; i++) {   /* first vertex stays first: flat colour comes from it */
            *o++ = vtx[i];
            *o++ = vtx[i + 1];
            *o++ = vtx[i + 2];
        }
    } else if (prim == GFX_TRIFAN) {
        for (i = 1; i + 1 < count; i++) {
            *o++ = vtx[0];
            *o++ = vtx[i];
            *o++ = vtx[i + 1];
        }
    } else {
        return;
    }
    g_nbatch += need;
}

static void vk_clear(const gfx_rect *rect, int color_too, uint32_t color, int depth_too, float depth)
{
    VkClearAttachment ca[2];
    VkClearRect cr;
    uint32_t n = 0;
    (void)rect;   /* the game only clears the whole screen */
    if (!v.ready || (!color_too && !depth_too))
        return;
    flush_batch();
    open_scene();
    memset(ca, 0, sizeof ca);
    if (color_too) {
        ca[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ca[n].colorAttachment = 0;
        ca[n].clearValue.color.float32[0] = ((color >> 16) & 0xFF) / 255.0f;
        ca[n].clearValue.color.float32[1] = ((color >> 8) & 0xFF) / 255.0f;
        ca[n].clearValue.color.float32[2] = (color & 0xFF) / 255.0f;
        ca[n].clearValue.color.float32[3] = 1.0f;
        n++;
    }
    if (depth_too) {
        ca[n].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        ca[n].clearValue.depthStencil.depth = depth;
        n++;
    }
    memset(&cr, 0, sizeof cr);
    cr.rect.extent.width = (uint32_t)v.scene_w;
    cr.rect.extent.height = (uint32_t)v.scene_h;
    cr.layerCount = 1;
    vkCmdClearAttachments(v.fr[v.fi].cmd, n, ca, 1, &cr);
}

/* ---------------------------------------------------------------- composing and presenting */

static uint32_t g_img;               /* the swapchain image of the composed frame */
static int g_have_img;               /* composed, not presented yet */

/* submit the frame (its pending present uses swap_done[g_img]) and move to the next one */
static void submit_frame(int with_image)
{
    frame *f = &v.fr[v.fi];
    VkSubmitInfo si;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (!f->recording)
        return;
    vkEndCommandBuffer(f->cmd);
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f->cmd;
    if (with_image) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f->acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &v.swap_done[g_img];
    }
    EnterCriticalSection(&v.qlock);
    vkQueueSubmit(v.queue, 1, &si, f->fence);
    LeaveCriticalSection(&v.qlock);
    f->recording = 0;
    f->submitted = 1;
    v.fi = (v.fi + 1) % FRAMES;
}

static void present_now(void)
{
    VkPresentInfoKHR pi;
    VkResult r;
    if (!g_have_img)
        return;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &v.swap_done[g_img];
    pi.swapchainCount = 1;
    pi.pSwapchains = &v.swap;
    pi.pImageIndices = &g_img;
    EnterCriticalSection(&v.qlock);
    r = vkQueuePresentKHR(v.queue, &pi);
    LeaveCriticalSection(&v.qlock);
    g_have_img = 0;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        v.swap_dirty = 1;
}

/* the UI for this frame (gfx_ui.h): its triangles, built once, and a new atlas uploaded before
   the swapchain pass when it changed */
static const gfx_ui_vert *g_ui_verts;
static int g_ui_n;

static void ui_prepare(void)
{
    const uint8_t *atlas;
    int aw, ah;
    unsigned gen;
    g_ui_n = gfx_ui_build(v.back_w, v.back_h, &g_ui_verts, &atlas, &aw, &ah, &gen);
    if (g_ui_n <= 0 || (v.ui_atlas && gen == v.ui_gen))
        return;
    if (v.ui_atlas)
        vk_tex_destroy(v.ui_atlas);
    v.ui_atlas = vk_tex_create_fmt(aw, ah, VK_FORMAT_R8_UNORM);
    if (v.ui_atlas)
        upload(v.ui_atlas, atlas, 1);
    v.ui_gen = gen;
}

/* ... drawn in the open swapchain pass */
static void ui_draw(VkCommandBuffer c)
{
    frame *f = &v.fr[v.fi];
    VkDeviceSize bytes, at;
    float inv[16];   /* (the whole push constant range: 64 bytes) */
    if (g_ui_n <= 0 || !v.ui_atlas)
        return;
    bytes = (VkDeviceSize)g_ui_n * sizeof(gfx_ui_vert);
    at = (f->vb.pos + 15) & ~(VkDeviceSize)15;
    if (at + bytes > f->vb.size)
        return;
    memcpy(f->vb.map + at, g_ui_verts, (size_t)bytes);
    f->vb.pos = at + bytes;
    memset(inv, 0, sizeof inv);
    inv[0] = 1.0f / v.back_w;
    inv[1] = 1.0f / v.back_h;
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, v.pipe_ui);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, v.layout, 0, 1, &v.set_blit_linear, 0, NULL);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, v.layout, 1, 1, &v.ui_atlas->set, 0, NULL);
    vkCmdPushConstants(c, v.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof inv, inv);
    vkCmdBindVertexBuffers(c, 0, 1, &f->vb.buf, &at);
    vkCmdDraw(c, (uint32_t)g_ui_n, 1, 0, 0);
}

static void vk_compose_target(int ti)
{
    target *t;
    VkCommandBuffer c;
    VkRenderPassBeginInfo rb;
    VkClearValue black;
    VkResult r;
    double sx, sy, s;
    float vw, vh;
    int cw, ch;
    if (!v.ready)
        return;
    vk_wait_flip();   /* the swapchain image may still hold a frame waiting for its moment */
    if (ti < 0 || ti > 1 || !v.tg_used[ti])
        ti = 0;
    end_pass();
    /* follow the window size (and a changed vsync) */
    gfx_win_client_size(v.hwnd, &cw, &ch);
    if (v.swap_dirty || (uint32_t)cw != v.swap_w || (uint32_t)ch != v.swap_h) {
        int ow, oh, nw, nh;
        scene_size(&ow, &oh);
        submit_frame(0);
        if (!make_swap())
            return;   /* minimised */
        scene_size(&nw, &nh);
        if (nw != ow || nh != oh) {
            release_scene();
            make_scene();
        }
    }
    ui_prepare();
    c = cmd();
    r = vkAcquireNextImageKHR(v.dev, v.swap, UINT64_MAX, v.fr[v.fi].acquired, VK_NULL_HANDLE, &g_img);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        v.swap_dirty = 1;
        return;
    }
    t = &v.tg[ti];
    if (t->fresh) {   /* nothing drawn since it was made: clear it once */
        int save = v.cur;
        v.cur = ti;
        open_scene();
        end_pass();
        v.cur = save;
    }
    if (t->resolved) {
        barrier(t->color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        barrier(t->resolved, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        {
            VkImageResolve rs;
            memset(&rs, 0, sizeof rs);
            rs.srcSubresource.aspectMask = rs.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            rs.srcSubresource.layerCount = rs.dstSubresource.layerCount = 1;
            rs.extent.width = (uint32_t)v.scene_w;
            rs.extent.height = (uint32_t)v.scene_h;
            rs.extent.depth = 1;
            vkCmdResolveImage(c, t->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->resolved,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rs);
        }
        barrier(t->color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        barrier(t->resolved, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    } else {
        barrier(t->color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    memset(&rb, 0, sizeof rb);
    memset(&black, 0, sizeof black);
    black.color.float32[3] = 1.0f;
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = v.pass_swap;
    rb.framebuffer = v.swap_fb[g_img];
    rb.renderArea.extent.width = v.swap_w;
    rb.renderArea.extent.height = v.swap_h;
    rb.clearValueCount = 1;
    rb.pClearValues = &black;
    vkCmdBeginRenderPass(c, &rb, VK_SUBPASS_CONTENTS_INLINE);
    /* keep the game's aspect (pillar/letterbox) unless told to stretch */
    sx = (double)v.back_w / v.app_w;
    sy = (double)v.back_h / v.app_h;
    s = sx < sy ? sx : sy;
    vw = v.opt.stretch ? (float)v.back_w : (float)(v.app_w * s);
    vh = v.opt.stretch ? (float)v.back_h : (float)(v.app_h * s);
    set_viewport((v.back_w - vw) / 2, (v.back_h - vh) / 2, vw, vh, v.back_w, v.back_h);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, v.pipe_blit);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, v.layout, 0, 1,
                            v.opt.nearest_upscale ? &v.set_blit_point : &v.set_blit_linear, 0, NULL);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, v.layout, 1, 1, &t->set, 0, NULL);
    vkCmdDraw(c, 3, 1, 0, 0);
    set_viewport(0, 0, (float)v.back_w, (float)v.back_h, v.back_w, v.back_h);
    ui_draw(c);   /* in-game menu, if open */
    vkCmdEndRenderPass(c);
    if (!t->resolved)
        barrier(t->color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    submit_frame(1);
    g_have_img = 1;
    v.bound_pipe = VK_NULL_HANDLE;
    v.state_dirty = v.cb_dirty = 1;
}

static void vk_present_target(int t)
{
    vk_compose_target(t);
    if (v.ready)
        present_now();
}

static void vk_present(void)
{
    vk_present_target(v.cur);
}

/* ---- showing a composed frame later, from a timer thread (as gfx_d3d11.c) ---- */
static HANDLE g_flip_thread, g_flip_go, g_flip_idle, g_flip_timer;
static volatile double g_flip_due;
static void (*g_flip_cb)(void);

static double flip_now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static DWORD WINAPI flip_thread(void *p)
{
    (void)p;
    for (;;) {
        double left;
        WaitForSingleObject(g_flip_go, INFINITE);
        left = g_flip_due - flip_now_ms();
        if (left > 0.6 && g_flip_timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((left - 0.5) * 10000.0);
            if (SetWaitableTimer(g_flip_timer, &due, 0, NULL, NULL, FALSE))
                WaitForSingleObject(g_flip_timer, INFINITE);
        }
        while (flip_now_ms() < g_flip_due)
            YieldProcessor();
        present_now();
        if (g_flip_cb)
            g_flip_cb();
        SetEvent(g_flip_idle);
    }
}

static void vk_wait_flip(void)
{
    if (g_flip_idle)
        WaitForSingleObject(g_flip_idle, INFINITE);
}

static void vk_flip_at(double due_ms, void (*on_flip)(void))
{
    if (!v.ready)
        return;
    if (!g_flip_thread) {
        g_flip_go = CreateEventW(NULL, FALSE, FALSE, NULL);
        g_flip_idle = CreateEventW(NULL, TRUE, TRUE, NULL);
        g_flip_timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        if (g_flip_go && g_flip_idle)
            g_flip_thread = CreateThread(NULL, 0, flip_thread, NULL, 0, NULL);
        if (g_flip_thread)
            SetThreadPriority(g_flip_thread, THREAD_PRIORITY_HIGHEST);
        log_printf("gfx: %s", g_flip_thread ? "game frames presented from a timer thread"
                                            : "timer thread failed, presenting on the game thread");
    }
    if (!g_flip_thread) {
        while (flip_now_ms() < due_ms)
            Sleep(0);
        present_now();
        if (on_flip)
            on_flip();
        return;
    }
    vk_wait_flip();
    g_flip_cb = on_flip;
    g_flip_due = due_ms;
    ResetEvent(g_flip_idle);
    SetEvent(g_flip_go);
}

/* ---------------------------------------------------------------- settings while running */

static const gfx_options *vk_get_options(void)
{
    return &v.opt;
}

static void vk_set_vsync(int on)
{
    if (on != v.opt.vsync) {
        v.opt.vsync = on;
        v.swap_dirty = 1;   /* another present mode: a new swapchain at the next frame */
    }
}

static void vk_set_nearest_upscale(int on)
{
    v.opt.nearest_upscale = on;
}

static void vk_set_anisotropy(int n)
{
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet w;
    if (!v.ready)
        return;
    end_pass();
    submit_frame(0);
    vk_wait_flip();
    vkDeviceWaitIdle(v.dev);
    v.opt.anisotropy = n;
    vkDestroySampler(v.dev, v.smp_linear, NULL);
    v.smp_linear = make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, (float)n);
    memset(&ii, 0, sizeof ii);
    ii.sampler = v.smp_linear;
    memset(&w, 0, sizeof w);
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = v.set_linear;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(v.dev, 1, &w, 0, NULL);
    v.state_dirty = 1;
}

/* resolution of the game's image and antialiasing: the scene targets (and the pipelines, whose
   sample count changes) are rebuilt */
static void vk_set_scene(int render_scale, int msaa)
{
    if (!v.ready || (render_scale == v.opt.render_scale && msaa == v.opt.msaa))
        return;
    end_pass();
    submit_frame(0);
    vk_wait_flip();
    vkDeviceWaitIdle(v.dev);
    v.opt.render_scale = render_scale;
    v.opt.msaa = msaa;
    release_scene();
    release_game_pipes();
    vkDestroyRenderPass(v.dev, v.pass_clear, NULL);
    vkDestroyRenderPass(v.dev, v.pass_load, NULL);
    v.samples = pick_samples(msaa);
    v.pass_clear = make_pass(VK_FORMAT_R8G8B8A8_UNORM, v.depth_fmt, v.samples, 1, 0);
    v.pass_load = make_pass(VK_FORMAT_R8G8B8A8_UNORM, v.depth_fmt, v.samples, 0, 0);
    make_scene();
    v.bound_pipe = VK_NULL_HANDLE;
    v.state_dirty = v.cb_dirty = 1;
}

static int vk_set_shader(const char *path)
{
    strcpy_s(v.opt.shader, sizeof v.opt.shader, path ? path : "");
    return !v.opt.shader[0];   /* presets aren't in this backend yet: only "none" works */
}

static void vk_use_target(int t)
{
    if (!v.ready || t < 0 || t > 1)
        return;
    if (!v.tg_used[t]) {
        v.tg_used[t] = 1;
        if (!make_target(t)) {
            log_printf("gfx: second scene target failed");
            v.tg_used[t] = 0;
            return;
        }
    }
    if (t != v.cur) {
        end_pass();
        v.cur = t;
    }
}

static void vk_finish_depth(int t)
{
    (void)t;   /* the effects depth (ReShade) isn't in this backend yet */
}

const gfx_backend gfx_backend_vulkan = {
    "vulkan",
    vk_init, vk_prepare_window, vk_ready, vk_set_mode,
    vk_tex_create, vk_tex_upload, vk_tex_destroy, vk_tex_size,
    vk_set_texture, vk_set_state, vk_draw, vk_clear,
    vk_present, vk_use_target, vk_present_target, vk_compose_target,
    vk_flip_at, vk_wait_flip, vk_get_options,
    vk_set_vsync, vk_set_nearest_upscale, vk_set_anisotropy, vk_set_scene, vk_set_shader,
    vk_finish_depth,
    1,   /* deep driver calls (pipeline compiles): never on the game's stacks */
};
#else
typedef int gfx_vk_unused;   /* built without Vulkan (no headers or glslc): nothing here */
#endif
