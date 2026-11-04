/* Android skeleton test (step A): SDL3 opens the window, Vulkan clears it to a slowly changing
 * colour. Proves the toolchain, the package and the device's Vulkan before the game's runtime
 * goes in. Logs to logcat (tag SDL/APP). */
#define VK_NO_PROTOTYPES
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <math.h>
#include <string.h>

#define VKF(name) static PFN_##name name;
VKF(vkCreateInstance) VKF(vkEnumeratePhysicalDevices) VKF(vkGetPhysicalDeviceProperties)
VKF(vkGetPhysicalDeviceQueueFamilyProperties) VKF(vkGetPhysicalDeviceSurfaceSupportKHR) VKF(vkCreateDevice)
VKF(vkGetDeviceProcAddr) VKF(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) VKF(vkGetPhysicalDeviceSurfaceFormatsKHR)
VKF(vkGetDeviceQueue) VKF(vkCreateSwapchainKHR) VKF(vkGetSwapchainImagesKHR) VKF(vkAcquireNextImageKHR)
VKF(vkQueuePresentKHR) VKF(vkQueueSubmit) VKF(vkCreateCommandPool) VKF(vkAllocateCommandBuffers)
VKF(vkBeginCommandBuffer) VKF(vkEndCommandBuffer) VKF(vkCmdPipelineBarrier) VKF(vkCmdClearColorImage)
VKF(vkCreateSemaphore) VKF(vkCreateFence) VKF(vkWaitForFences) VKF(vkResetFences) VKF(vkResetCommandBuffer)
VKF(vkDeviceWaitIdle)

static void fail(const char *what)
{
    SDL_Log("hello: %s failed", what);
}

static void barrier(VkCommandBuffer c, VkImage img, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_ACCESS_TRANSFER_WRITE_BIT : 0;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &b);
}

int main(int argc, char **argv)
{
    SDL_Window *win;
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance inst;
    VkSurfaceKHR surf;
    VkPhysicalDevice phys[8], pd = VK_NULL_HANDLE;
    VkDevice dev;
    VkQueue queue;
    VkSwapchainKHR swap;
    VkImage imgs[8];
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkSemaphore acquired, done;
    VkFence fence;
    uint32_t n = 8, i, qfam = 0, nimg = 8, ext_n = 0, frames = 0;
    const char *const *ext;
    float prio = 1.0f;
    int running = 1;
    (void)argc; (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fail("SDL_Init");
        return 1;
    }
    win = SDL_CreateWindow("Mega Man Legends", 0, 0, SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN);
    if (!win) {
        fail("SDL_CreateWindow");
        return 1;
    }
    gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!gipa) {
        fail("Vulkan loader");
        return 1;
    }
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    ext = SDL_Vulkan_GetInstanceExtensions(&ext_n);
    {
        VkApplicationInfo ai;
        VkInstanceCreateInfo ii;
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        ai.pApplicationName = "mml hello";
        ai.apiVersion = VK_API_VERSION_1_1;
        memset(&ii, 0, sizeof ii);
        ii.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ii.pApplicationInfo = &ai;
        ii.enabledExtensionCount = ext_n;
        ii.ppEnabledExtensionNames = ext;
        if (vkCreateInstance(&ii, NULL, &inst) != VK_SUCCESS) {
            fail("vkCreateInstance");
            return 1;
        }
    }
#define VKI(name) name = (PFN_##name)gipa(inst, #name);
    VKI(vkEnumeratePhysicalDevices) VKI(vkGetPhysicalDeviceProperties) VKI(vkGetPhysicalDeviceQueueFamilyProperties)
    VKI(vkGetPhysicalDeviceSurfaceSupportKHR) VKI(vkCreateDevice) VKI(vkGetDeviceProcAddr)
    VKI(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) VKI(vkGetPhysicalDeviceSurfaceFormatsKHR)
    if (!SDL_Vulkan_CreateSurface(win, inst, NULL, &surf)) {
        fail("SDL_Vulkan_CreateSurface");
        return 1;
    }
    vkEnumeratePhysicalDevices(inst, &n, phys);
    for (i = 0; i < n && !pd; i++) {
        VkQueueFamilyProperties qf[16];
        uint32_t nq = 16, q;
        vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &nq, qf);
        for (q = 0; q < nq; q++) {
            VkBool32 ok = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(phys[i], q, surf, &ok);
            if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && ok) {
                pd = phys[i];
                qfam = q;
                break;
            }
        }
    }
    if (!pd) {
        fail("finding a GPU");
        return 1;
    }
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd, &p);
        SDL_Log("hello: Vulkan device %s (API %u.%u.%u)", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
                VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
    }
    {
        static const char *const dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        VkDeviceQueueCreateInfo qi;
        VkDeviceCreateInfo di;
        memset(&qi, 0, sizeof qi);
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = qfam;
        qi.queueCount = 1;
        qi.pQueuePriorities = &prio;
        memset(&di, 0, sizeof di);
        di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        di.enabledExtensionCount = 1;
        di.ppEnabledExtensionNames = dext;
        if (vkCreateDevice(pd, &di, NULL, &dev) != VK_SUCCESS) {
            fail("vkCreateDevice");
            return 1;
        }
    }
#define VKD(name) name = (PFN_##name)vkGetDeviceProcAddr(dev, #name);
    VKD(vkGetDeviceQueue) VKD(vkCreateSwapchainKHR) VKD(vkGetSwapchainImagesKHR) VKD(vkAcquireNextImageKHR)
    VKD(vkQueuePresentKHR) VKD(vkQueueSubmit) VKD(vkCreateCommandPool) VKD(vkAllocateCommandBuffers)
    VKD(vkBeginCommandBuffer) VKD(vkEndCommandBuffer) VKD(vkCmdPipelineBarrier) VKD(vkCmdClearColorImage)
    VKD(vkCreateSemaphore) VKD(vkCreateFence) VKD(vkWaitForFences) VKD(vkResetFences) VKD(vkResetCommandBuffer)
    VKD(vkDeviceWaitIdle)
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    {
        VkSurfaceCapabilitiesKHR caps;
        VkSurfaceFormatKHR fmts[32];
        uint32_t nf = 32;
        VkSwapchainCreateInfoKHR si;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps);
        vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, fmts);
        memset(&si, 0, sizeof si);
        si.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        si.surface = surf;
        si.minImageCount = caps.minImageCount + 1;
        if (caps.maxImageCount && si.minImageCount > caps.maxImageCount)
            si.minImageCount = caps.maxImageCount;
        si.imageFormat = fmts[0].format;
        si.imageColorSpace = fmts[0].colorSpace;
        si.imageExtent = caps.currentExtent;
        si.imageArrayLayers = 1;
        si.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        si.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        si.preTransform = caps.currentTransform;
        si.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                                ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        si.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        si.clipped = VK_TRUE;
        if (vkCreateSwapchainKHR(dev, &si, NULL, &swap) != VK_SUCCESS) {
            fail("vkCreateSwapchainKHR");
            return 1;
        }
        SDL_Log("hello: swapchain %ux%u, format %d", si.imageExtent.width, si.imageExtent.height, (int)si.imageFormat);
    }
    vkGetSwapchainImagesKHR(dev, swap, &nimg, imgs);
    {
        VkCommandPoolCreateInfo pi;
        VkCommandBufferAllocateInfo ai;
        VkSemaphoreCreateInfo semi;
        VkFenceCreateInfo fi;
        memset(&pi, 0, sizeof pi);
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = qfam;
        vkCreateCommandPool(dev, &pi, NULL, &pool);
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev, &ai, &cmd);
        memset(&semi, 0, sizeof semi);
        semi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(dev, &semi, NULL, &acquired);
        vkCreateSemaphore(dev, &semi, NULL, &done);
        memset(&fi, 0, sizeof fi);
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(dev, &fi, NULL, &fence);
    }
    SDL_Log("hello: running");
    while (running) {
        SDL_Event e;
        uint32_t img;
        VkCommandBufferBeginInfo bi;
        VkClearColorValue col;
        VkImageSubresourceRange range;
        VkSubmitInfo si;
        VkPresentInfoKHR pi;
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        float t = SDL_GetTicks() / 1000.0f;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_TERMINATING)
                running = 0;
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        if (vkAcquireNextImageKHR(dev, swap, UINT64_MAX, acquired, VK_NULL_HANDLE, &img) < 0) {
            SDL_Delay(16);   /* (in the background: the surface is gone) */
            continue;
        }
        vkResetFences(dev, 1, &fence);
        vkResetCommandBuffer(cmd, 0);
        memset(&bi, 0, sizeof bi);
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &bi);
        barrier(cmd, imgs[img], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        col.float32[0] = 0.10f + 0.10f * sinf(t);
        col.float32[1] = 0.25f + 0.15f * sinf(t * 0.7f);
        col.float32[2] = 0.55f + 0.25f * sinf(t * 0.5f);
        col.float32[3] = 1.0f;
        memset(&range, 0, sizeof range);
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        vkCmdClearColorImage(cmd, imgs[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &range);
        barrier(cmd, imgs[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        vkEndCommandBuffer(cmd);
        memset(&si, 0, sizeof si);
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &acquired;
        si.pWaitDstStageMask = &stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &done;
        vkQueueSubmit(queue, 1, &si, fence);
        memset(&pi, 0, sizeof pi);
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &done;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swap;
        pi.pImageIndices = &img;
        vkQueuePresentKHR(queue, &pi);
        if (++frames % 300 == 0)
            SDL_Log("hello: %u frames", frames);
    }
    vkDeviceWaitIdle(dev);
    SDL_Log("hello: done");
    return 0;
}
