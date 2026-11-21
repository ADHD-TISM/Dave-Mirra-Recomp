/* The Vulkan backend (docs/renderer-roadmap.md, phase 4): shows the CPU picture in the window. Plainly, the picture is copied into
   an image and blitted into the swapchain image with the nearest or the linear filter (vkCmdBlitImage), black bars included (the
   swapchain image is cleared first). With FSR on, two render passes do it instead: EASU into an image of the picture's final size,
   then RCAS into the swapchain image (the SPIR-V is in gfx_vk_spv.inc, made from runtime/shaders by tools/shaders.py). Not here
   yet: the GPU hi-res renderer (r_init says no, so gpu_gl keeps the CPU picture), validation layers.
   Vulkan is opened at run time (plat_vk_loader), so a machine without it just fails init and the next backend is tried. */
#define VK_NO_PROTOTYPES
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "vulkan/vulkan.h"
#include "hw.h"
#include "host.h"
#include "gfx.h"
#include "gfx_vk_spv.inc"

#define VK_FUNCS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) X(vkEnumerateDeviceExtensionProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkDestroySurfaceKHR) X(vkGetDeviceQueue) X(vkDestroyDevice) X(vkDeviceWaitIdle) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) \
    X(vkQueueSubmit) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
    X(vkCmdBlitImage) X(vkCmdClearColorImage) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateFence) X(vkDestroyFence) \
    X(vkWaitForFences) X(vkResetFences) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) \
    X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) \
    X(vkDestroyFramebuffer) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor)
#define X(n) static PFN_##n n;
VK_FUNCS(X)
#undef X

#define MAX_IMAGES 16
static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst; static VkSurfaceKHR surf; static VkPhysicalDevice phys; static VkDevice dev; static VkQueue queue;
static uint32_t qfam; static VkPhysicalDeviceMemoryProperties memprops;
static VkSwapchainKHR swap; static VkImage simg[MAX_IMAGES]; static VkSemaphore rel[MAX_IMAGES]; static uint32_t nimg;
static VkFormat sfmt; static VkExtent2D sext; static int req_w, req_h, sc_dirty;   /* req_*: the client size the swapchain was asked for */ static VkPresentModeKHR pmode;
static int blit_linear;                                 /* the picture format can be filtered linearly in a blit */
static VkCommandPool pool; static VkCommandBuffer cb; static VkSemaphore acq; static VkFence fence;
static VkImage pic; static VkDeviceMemory pic_mem; static VkImageView pic_view; static int pic_w, pic_h;   /* the CPU picture, on the device */
static VkBuffer stage; static VkDeviceMemory stage_mem; static void *stage_map; /* ... and where it is copied from */
static VkBuffer rb; static VkDeviceMemory rb_mem; static void *rb_map; static size_t rb_size;   /* the window read-back (--shot-window) */
static uint32_t *grab; static int grab_w, grab_h, grab_ok;
/* FSR: two passes. fsr_possible: the device lets the swapchain image be drawn into; fsr_ok: the pipelines were made. */
static int fsr_possible, fsr_ok;
static VkRenderPass rp_a, rp_b; static VkSampler samp; static VkDescriptorSetLayout dsl; static VkDescriptorPool dpool; static VkDescriptorSet dset[2];
static VkPipelineLayout playout; static VkPipeline pipe_easu, pipe_rcas;
static VkImage fimg; static VkDeviceMemory fimg_mem; static VkImageView fview; static VkFramebuffer ffb; static int fimg_w, fimg_h;   /* EASU's output */
static VkImageView sview[MAX_IMAGES]; static VkFramebuffer sfb[MAX_IMAGES];                                                    /* RCAS draws into these */
typedef struct { float in[2], out[2], origin[2], con; } FsrPush;
_Static_assert(sizeof(FsrPush) == 28, "FSR push constant layout");

static void disable_fsr(void) {
    fsr_ok = 0;
    fprintf(stderr, "[video] Vulkan: FSR is unavailable; using the plain blit\n");
}

static int load_funcs(VkInstance i) {
#define X(n) if (!(n = (PFN_##n)gipa(i, #n))) { fprintf(stderr, "[video] Vulkan: %s is missing\n", #n); return 0; }
    VK_FUNCS(X)
#undef X
    return 1;
}
static int find_mem(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}
static int make_buffer(size_t size, VkBufferUsageFlags use, VkBuffer *b, VkDeviceMemory *m, void **map) {
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = use, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (vkCreateBuffer(dev, &bi, NULL, b) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, *b, &mr);
    int t = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t < 0) return 0;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)t };
    if (vkAllocateMemory(dev, &ai, NULL, m) != VK_SUCCESS) return 0;
    if (vkBindBufferMemory(dev, *b, *m, 0) != VK_SUCCESS) return 0;
    return vkMapMemory(dev, *m, 0, VK_WHOLE_SIZE, 0, map) == VK_SUCCESS;
}
static void free_buffer(VkBuffer *b, VkDeviceMemory *m, void **map) {
    if (*map) vkUnmapMemory(dev, *m);
    if (*b) vkDestroyBuffer(dev, *b, NULL);
    if (*m) vkFreeMemory(dev, *m, NULL);
    *b = VK_NULL_HANDLE; *m = VK_NULL_HANDLE; *map = NULL;
}
static void free_picture(void) {
    free_buffer(&stage, &stage_mem, &stage_map);
    if (pic_view) vkDestroyImageView(dev, pic_view, NULL);
    if (pic) vkDestroyImage(dev, pic, NULL);
    if (pic_mem) vkFreeMemory(dev, pic_mem, NULL);
    pic_view = VK_NULL_HANDLE; pic = VK_NULL_HANDLE; pic_mem = VK_NULL_HANDLE; pic_w = pic_h = 0;
}
static int make_picture(int w, int h) {   /* an image for the CPU picture (0x00RRGGBB is B8G8R8A8 in memory) and the buffer it is copied from */
    vkDeviceWaitIdle(dev);
    free_picture();
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = { (uint32_t)w, (uint32_t)h, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (fsr_ok) ii.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (vkCreateImage(dev, &ii, NULL, &pic) != VK_SUCCESS) goto picture_failed;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, pic, &mr);
    int t = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t < 0) t = find_mem(mr.memoryTypeBits, 0);
    if (t < 0) goto picture_failed;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)t };
    if (vkAllocateMemory(dev, &ai, NULL, &pic_mem) != VK_SUCCESS || vkBindImageMemory(dev, pic, pic_mem, 0) != VK_SUCCESS) goto picture_failed;
    if (!make_buffer((size_t)w * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stage, &stage_mem, &stage_map)) return 0;
    pic_w = w; pic_h = h;
    if (fsr_ok) {
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = pic, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        if (vkCreateImageView(dev, &vi, NULL, &pic_view) != VK_SUCCESS) {
            disable_fsr();
            return 1;
        }
        VkDescriptorImageInfo di = { .sampler = samp, .imageView = pic_view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset[0], .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &di };
        vkUpdateDescriptorSets(dev, 1, &wr, 0, NULL);
    }
    return 1;
picture_failed:
    free_picture();
    if (fsr_ok) {
        disable_fsr();
        return make_picture(w, h);
    }
    return 0;
}
static void free_fsr_target(void) {
    if (ffb) vkDestroyFramebuffer(dev, ffb, NULL);
    if (fview) vkDestroyImageView(dev, fview, NULL);
    if (fimg) vkDestroyImage(dev, fimg, NULL);
    if (fimg_mem) vkFreeMemory(dev, fimg_mem, NULL);
    ffb = VK_NULL_HANDLE; fview = VK_NULL_HANDLE; fimg = VK_NULL_HANDLE; fimg_mem = VK_NULL_HANDLE; fimg_w = fimg_h = 0;
}
static int make_fsr_target(int w, int h) {   /* what EASU draws, at the size the picture will have in the window */
    vkDeviceWaitIdle(dev);
    free_fsr_target();
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = { (uint32_t)w, (uint32_t)h, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (vkCreateImage(dev, &ii, NULL, &fimg) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, fimg, &mr);
    int t = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t < 0) t = find_mem(mr.memoryTypeBits, 0);
    if (t < 0) return 0;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)t };
    if (vkAllocateMemory(dev, &ai, NULL, &fimg_mem) != VK_SUCCESS || vkBindImageMemory(dev, fimg, fimg_mem, 0) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = fimg, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    if (vkCreateImageView(dev, &vi, NULL, &fview) != VK_SUCCESS) return 0;
    VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp_a, .attachmentCount = 1, .pAttachments = &fview,
        .width = (uint32_t)w, .height = (uint32_t)h, .layers = 1 };
    if (vkCreateFramebuffer(dev, &fi, NULL, &ffb) != VK_SUCCESS) return 0;
    VkDescriptorImageInfo di = { .sampler = samp, .imageView = fview, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset[1], .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &di };
    vkUpdateDescriptorSets(dev, 1, &wr, 0, NULL);
    fimg_w = w; fimg_h = h;
    return 1;
}

static void destroy_swapchain_views(void) {
    for (uint32_t i = 0; i < MAX_IMAGES; i++) {
        if (sfb[i]) { vkDestroyFramebuffer(dev, sfb[i], NULL); sfb[i] = VK_NULL_HANDLE; }
        if (sview[i]) { vkDestroyImageView(dev, sview[i], NULL); sview[i] = VK_NULL_HANDLE; }
    }
}
static void destroy_swapchain(void) {
    destroy_swapchain_views();
    for (uint32_t i = 0; i < nimg; i++) if (rel[i]) { vkDestroySemaphore(dev, rel[i], NULL); rel[i] = VK_NULL_HANDLE; }
    if (swap) { vkDestroySwapchainKHR(dev, swap, NULL); swap = VK_NULL_HANDLE; }
    nimg = 0;
}
/* (re)make the swapchain at the window's size; 0 when it cannot be made now (a minimised window has no size) */
static int make_swapchain(int cw, int ch) {
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surf, &caps) != VK_SUCCESS) return 0;
    VkExtent2D e = caps.currentExtent;
    if (e.width == 0xFFFFFFFFu) {
        e.width = (uint32_t)cw < caps.minImageExtent.width ? caps.minImageExtent.width : (uint32_t)cw > caps.maxImageExtent.width ? caps.maxImageExtent.width : (uint32_t)cw;
        e.height = (uint32_t)ch < caps.minImageExtent.height ? caps.minImageExtent.height : (uint32_t)ch > caps.maxImageExtent.height ? caps.maxImageExtent.height : (uint32_t)ch;
    }
    if (!e.width || !e.height) return 0;
    uint32_t n = caps.minImageCount + 1;
    if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;
    if (n > MAX_IMAGES) n = MAX_IMAGES;
    vkDeviceWaitIdle(dev);
    VkSwapchainKHR old = swap;
    destroy_swapchain_views();
    for (uint32_t i = 0; i < nimg; i++) if (rel[i]) { vkDestroySemaphore(dev, rel[i], NULL); rel[i] = VK_NULL_HANDLE; }
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;   /* the picture has no alpha: opaque if offered, else whatever the surface takes */
    if (!(caps.supportedCompositeAlpha & alpha)) {
        static const VkCompositeAlphaFlagBitsKHR alt[3] = { VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR };
        for (int k = 0; k < 3; k++) if (caps.supportedCompositeAlpha & alt[k]) { alpha = alt[k]; break; }
    }
    VkSwapchainCreateInfoKHR si = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf, .minImageCount = n, .imageFormat = sfmt,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, .imageExtent = e, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform, .compositeAlpha = alpha, .presentMode = pmode, .clipped = VK_TRUE, .oldSwapchain = old };
    VkSwapchainKHR ns = VK_NULL_HANDLE;
    if (fsr_ok && !(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) disable_fsr();
    if (fsr_ok) si.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkResult r = vkCreateSwapchainKHR(dev, &si, NULL, &ns);
    if (old) vkDestroySwapchainKHR(dev, old, NULL);
    swap = VK_NULL_HANDLE; nimg = 0;
    if (r != VK_SUCCESS && fsr_ok) {
        disable_fsr();
        si.imageUsage &= ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        si.oldSwapchain = VK_NULL_HANDLE;
        r = vkCreateSwapchainKHR(dev, &si, NULL, &ns);
    }
    if (r != VK_SUCCESS) { fprintf(stderr, "[video] Vulkan: cannot make a swapchain (%d)\n", (int)r); return 0; }
    swap = ns;
    uint32_t have = 0;
    if (vkGetSwapchainImagesKHR(dev, swap, &have, NULL) != VK_SUCCESS || !have || have > MAX_IMAGES) {
        fprintf(stderr, "[video] Vulkan: the swapchain has %u images (at most %d are handled)\n", have, MAX_IMAGES);
        return 0;   /* swap stays set, so the next frame's rebuild retires it */
    }
    nimg = have;
    if (vkGetSwapchainImagesKHR(dev, swap, &nimg, simg) != VK_SUCCESS) { nimg = 0; return 0; }
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < nimg; i++) if (vkCreateSemaphore(dev, &sci, NULL, &rel[i]) != VK_SUCCESS) return 0;
    if (fsr_ok) for (uint32_t i = 0; i < nimg; i++) {   /* RCAS draws straight into the swapchain images */
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = simg[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sfmt,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp_b, .attachmentCount = 1, .pAttachments = &sview[i],
            .width = e.width, .height = e.height, .layers = 1 };
        if (vkCreateImageView(dev, &vi, NULL, &sview[i]) != VK_SUCCESS || vkCreateFramebuffer(dev, &fi, NULL, &sfb[i]) != VK_SUCCESS) {
            destroy_swapchain_views();
            disable_fsr();
            break;
        }
    }
    sext = e; req_w = cw; req_h = ch; sc_dirty = 0;
    return 1;
}

static void teardown(void) {
    if (dev) { vkDeviceWaitIdle(dev); free_picture(); free_fsr_target(); free_buffer(&rb, &rb_mem, &rb_map); destroy_swapchain(); }
    if (dev) {
        if (pipe_easu) vkDestroyPipeline(dev, pipe_easu, NULL);
        if (pipe_rcas) vkDestroyPipeline(dev, pipe_rcas, NULL);
        if (playout) vkDestroyPipelineLayout(dev, playout, NULL);
        if (dpool) vkDestroyDescriptorPool(dev, dpool, NULL);
        if (dsl) vkDestroyDescriptorSetLayout(dev, dsl, NULL);
        if (samp) vkDestroySampler(dev, samp, NULL);
        if (rp_a) vkDestroyRenderPass(dev, rp_a, NULL);
        if (rp_b) vkDestroyRenderPass(dev, rp_b, NULL);
        pipe_easu = pipe_rcas = VK_NULL_HANDLE; playout = VK_NULL_HANDLE; dpool = VK_NULL_HANDLE; dsl = VK_NULL_HANDLE;
        samp = VK_NULL_HANDLE; rp_a = rp_b = VK_NULL_HANDLE; fsr_ok = 0;
    }
    if (dev && fence) vkDestroyFence(dev, fence, NULL);
    if (dev && acq) vkDestroySemaphore(dev, acq, NULL);
    if (dev && pool) vkDestroyCommandPool(dev, pool, NULL);
    fence = VK_NULL_HANDLE; acq = VK_NULL_HANDLE; pool = VK_NULL_HANDLE;
    if (dev) { vkDestroyDevice(dev, NULL); dev = VK_NULL_HANDLE; }
    if (inst && surf) vkDestroySurfaceKHR(inst, surf, NULL);
    surf = VK_NULL_HANDLE;
    if (inst) { vkDestroyInstance(inst, NULL); inst = VK_NULL_HANDLE; }
}

static int pick_device(void) {
    uint32_t n = 0; VkPhysicalDevice pd[16];
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    if (n > 16) n = 16;
    if (!n || vkEnumeratePhysicalDevices(inst, &n, pd) < 0) { fprintf(stderr, "[video] Vulkan: no graphics device\n"); return 0; }
    int best = -1; uint32_t best_q = 0; VkFormat best_fmt = VK_FORMAT_UNDEFINED; VkPresentModeKHR best_pm = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t d = 0; d < n; d++) {
        VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(pd[d], &pr);
        uint32_t ne = 0; VkExtensionProperties ex[256]; int has_swap = 0;
        vkEnumerateDeviceExtensionProperties(pd[d], NULL, &ne, NULL);
        if (ne > 256) ne = 256;
        if (vkEnumerateDeviceExtensionProperties(pd[d], NULL, &ne, ex) >= 0)
            for (uint32_t k = 0; k < ne; k++) if (!strcmp(ex[k].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) has_swap = 1;
        if (!has_swap) continue;
        uint32_t nq = 0; VkQueueFamilyProperties qp[16];
        vkGetPhysicalDeviceQueueFamilyProperties(pd[d], &nq, NULL);
        if (nq > 16) nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(pd[d], &nq, qp);
        int fam = -1;
        for (uint32_t q = 0; q < nq && fam < 0; q++) {
            VkBool32 ok = VK_FALSE;
            if ((qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && vkGetPhysicalDeviceSurfaceSupportKHR(pd[d], q, surf, &ok) == VK_SUCCESS && ok) fam = (int)q;
        }
        if (fam < 0) continue;
        VkSurfaceCapabilitiesKHR caps;
        const VkImageUsageFlags need = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd[d], surf, &caps) != VK_SUCCESS || (caps.supportedUsageFlags & need) != need) continue;
        int can_draw = (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
        uint32_t nf = 0; VkSurfaceFormatKHR sf[64]; VkFormat fmt = VK_FORMAT_UNDEFINED;   /* plain 8-bit formats only: an sRGB one would change the colours */
        vkGetPhysicalDeviceSurfaceFormatsKHR(pd[d], surf, &nf, NULL);
        if (nf > 64) nf = 64;
        if (vkGetPhysicalDeviceSurfaceFormatsKHR(pd[d], surf, &nf, sf) < 0) continue;
        for (uint32_t k = 0; k < nf; k++) if (sf[k].format == VK_FORMAT_B8G8R8A8_UNORM && sf[k].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) fmt = sf[k].format;
        for (uint32_t k = 0; k < nf && fmt == VK_FORMAT_UNDEFINED; k++) if (sf[k].format == VK_FORMAT_R8G8B8A8_UNORM && sf[k].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) fmt = sf[k].format;
        if (fmt == VK_FORMAT_UNDEFINED) continue;
        VkFormatProperties fp, pp;
        vkGetPhysicalDeviceFormatProperties(pd[d], fmt, &fp); vkGetPhysicalDeviceFormatProperties(pd[d], VK_FORMAT_B8G8R8A8_UNORM, &pp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) || !(pp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) continue;
        const VkFormatFeatureFlags fsr_features = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        can_draw = can_draw && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
            && (pp.optimalTilingFeatures & fsr_features) == fsr_features;
        int score = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (score > best) {
            best = score; phys = pd[d]; best_q = (uint32_t)fam; best_fmt = fmt;
            fsr_possible = can_draw;
            blit_linear = (pp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
            uint32_t np = 0; VkPresentModeKHR pm[8]; best_pm = VK_PRESENT_MODE_FIFO_KHR;   /* the game paces itself: do not wait for the monitor if that can be avoided */
            vkGetPhysicalDeviceSurfacePresentModesKHR(pd[d], surf, &np, NULL);
            if (np > 8) np = 8;
            if (vkGetPhysicalDeviceSurfacePresentModesKHR(pd[d], surf, &np, pm) >= 0) {
                for (uint32_t k = 0; k < np; k++) if (pm[k] == VK_PRESENT_MODE_MAILBOX_KHR && best_pm == VK_PRESENT_MODE_FIFO_KHR) best_pm = pm[k];
                for (uint32_t k = 0; k < np; k++) if (pm[k] == VK_PRESENT_MODE_IMMEDIATE_KHR) best_pm = pm[k];
            }
            fprintf(stderr, "[video] Vulkan: %s\n", pr.deviceName);
        }
    }
    if (best < 0) { fprintf(stderr, "[video] Vulkan: no device can show the picture (it needs a swapchain and 8-bit blits)\n"); return 0; }
    qfam = best_q; sfmt = best_fmt; pmode = best_pm;
    return 1;
}

static VkShaderModule make_module(const uint32_t *code, size_t words) {
    VkShaderModuleCreateInfo mi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = words * 4, .pCode = code };
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(dev, &mi, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}
static VkPipeline make_pipeline(VkShaderModule vs, VkShaderModule fs, VkRenderPass rp) {
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState ba = { .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
    VkPipelineColorBlendStateCreateInfo cb2 = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb2, .pDynamicState = &ds,
        .layout = playout, .renderPass = rp, .subpass = 0 };
    VkPipeline pl = VK_NULL_HANDLE;
    return vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gi, NULL, &pl) == VK_SUCCESS ? pl : VK_NULL_HANDLE;
}
static VkRenderPass make_pass(VkFormat fmt, VkImageLayout final_layout, int clear) {
    VkAttachmentDescription at = { .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = final_layout };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref };
    VkSubpassDependency dep[2] = {
        { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0, .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT },
        { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL, .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT } };
    VkRenderPassCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &at, .subpassCount = 1, .pSubpasses = &sp,
        .dependencyCount = 2, .pDependencies = dep };
    VkRenderPass rp = VK_NULL_HANDLE;
    return vkCreateRenderPass(dev, &ri, NULL, &rp) == VK_SUCCESS ? rp : VK_NULL_HANDLE;
}
/* the FSR pipelines; on any failure FSR stays off and the plain blit is used */
static int fsr_setup(void) {
    VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    VkDescriptorSetLayoutBinding lb = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &lb };
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
    VkDescriptorPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &ps };
    if (vkCreateSampler(dev, &si, NULL, &samp) != VK_SUCCESS || vkCreateDescriptorSetLayout(dev, &li, NULL, &dsl) != VK_SUCCESS
        || vkCreateDescriptorPool(dev, &pi, NULL, &dpool) != VK_SUCCESS) return 0;
    VkDescriptorSetLayout both[2] = { dsl, dsl };
    VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 2, .pSetLayouts = both };
    if (vkAllocateDescriptorSets(dev, &ai, dset) != VK_SUCCESS) return 0;
    VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FsrPush) };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
    if (vkCreatePipelineLayout(dev, &pli, NULL, &playout) != VK_SUCCESS) return 0;
    rp_a = make_pass(VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0);
    rp_b = make_pass(sfmt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1);
    VkShaderModule vs = make_module(spv_fullscreen_vert, sizeof spv_fullscreen_vert / 4), fe = make_module(spv_fsr_easu_frag, sizeof spv_fsr_easu_frag / 4),
                   fr = make_module(spv_fsr_rcas_frag, sizeof spv_fsr_rcas_frag / 4);
    if (rp_a && rp_b && vs && fe && fr) { pipe_easu = make_pipeline(vs, fe, rp_a); pipe_rcas = make_pipeline(vs, fr, rp_b); }
    if (vs) vkDestroyShaderModule(dev, vs, NULL);
    if (fe) vkDestroyShaderModule(dev, fe, NULL);
    if (fr) vkDestroyShaderModule(dev, fr, NULL);
    return pipe_easu && pipe_rcas;
}

static int setup(void) {
    gipa = (PFN_vkGetInstanceProcAddr)plat_vk_loader();
    if (!gipa) { fprintf(stderr, "[video] no Vulkan loader\n"); return 0; }
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    const char *ext[8]; int ne = plat_vk_extensions(ext, 8);
    if (!create || ne <= 0) { fprintf(stderr, "[video] Vulkan cannot show a window here\n"); return 0; }
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "dmrecomp", .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai, .enabledExtensionCount = (uint32_t)ne, .ppEnabledExtensionNames = ext };
    VkResult r = create(&ci, NULL, &inst);
    if (r != VK_SUCCESS) { fprintf(stderr, "[video] Vulkan: cannot make an instance (%d)\n", (int)r); inst = VK_NULL_HANDLE; return 0; }
    if (!load_funcs(inst)) return 0;
    uint64_t s64 = 0;
    if (!plat_vk_surface(inst, &s64)) { fprintf(stderr, "[video] Vulkan: cannot make a surface for the window\n"); return 0; }
    surf = (VkSurfaceKHR)s64;
    if (!pick_device()) return 0;
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio };
    const char *dext[1] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi, .enabledExtensionCount = 1, .ppEnabledExtensionNames = dext };
    r = vkCreateDevice(phys, &di, NULL, &dev);
    if (r != VK_SUCCESS) { fprintf(stderr, "[video] Vulkan: cannot make a device (%d)\n", (int)r); dev = VK_NULL_HANDLE; return 0; }
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfam };
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    if (vkCreateCommandPool(dev, &pi, NULL, &pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo ba = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    if (vkAllocateCommandBuffers(dev, &ba, &cb) != VK_SUCCESS || vkCreateSemaphore(dev, &sci, NULL, &acq) != VK_SUCCESS || vkCreateFence(dev, &fi, NULL, &fence) != VK_SUCCESS) return 0;
    fsr_ok = fsr_possible && fsr_setup();   /* sets fsr_ok before the swapchain, which makes the views FSR draws into */
    if (!fsr_ok) disable_fsr();
    int w, h; plat_client_size(&w, &h);
    if (w <= 0 || h <= 0) { w = 640; h = 480; }   /* the swapchain is remade at the real size on the first frame if this is not it */
    if (!make_swapchain(w, h)) return 0;
    return 1;
}

static int vk_init(int w, int h, const char *title) {
    if (!plat_surface_open(PLAT_SURF_VK, w, h, title)) return 0;
    if (!setup()) { teardown(); plat_surface_close(); return 0; }
    plat_surface_show();
    return 1;
}

static void barrier(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

/* after an acquire that cannot be used or a failed submit: the fence is reset and the acquire semaphore may be signalled with
   nothing waiting on it, so both are made new (the device is idle first) and the swapchain is rebuilt */
static void recover_sync(void) {
    vkDeviceWaitIdle(dev);
    if (fence) vkDestroyFence(dev, fence, NULL);
    if (acq) vkDestroySemaphore(dev, acq, NULL);
    fence = VK_NULL_HANDLE; acq = VK_NULL_HANDLE;
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (vkCreateFence(dev, &fi, NULL, &fence) != VK_SUCCESS || vkCreateSemaphore(dev, &sci, NULL, &acq) != VK_SUCCESS) fprintf(stderr, "[video] Vulkan: cannot recover after a failed frame\n");
    sc_dirty = 1;
}

static void vk_present(const GfxFrame *f) {
    grab_ok = 0;
    if (f->cw <= 0 || f->ch <= 0) return;
    if (sc_dirty || !swap || f->cw != req_w || f->ch != req_h) {
        if (!make_swapchain(f->cw, f->ch)) return;   /* minimised: nothing to show */
    }
    if (f->fb_w != pic_w || f->fb_h != pic_h) { if (!make_picture(f->fb_w, f->fb_h)) { free_picture(); return; } }
    /* the picture is placed like the OpenGL one: its offset from the bottom is (ch - h) / 2, so from the top it is the rest */
    int w = f->w < (int)sext.width ? f->w : (int)sext.width, h = f->h < (int)sext.height ? f->h : (int)sext.height;
    int x0 = ((int)sext.width - w) / 2, y0 = (int)sext.height - h - ((int)sext.height - h) / 2;
    int fsr = fsr_ok && f->upscale && f->w > f->fb_w && f->h > f->fb_h
        && w == f->w && h == f->h;
    if (fsr && (fimg_w != w || fimg_h != h) && !make_fsr_target(w, h)) {
        free_fsr_target();
        disable_fsr();
        fsr = 0;
    }
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    memcpy(stage_map, f->fb, (size_t)f->fb_w * f->fb_h * 4);
    uint32_t idx = 0;
    VkResult r = vkAcquireNextImageKHR(dev, swap, UINT64_MAX, acq, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { sc_dirty = 1; return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return;
    if (idx >= nimg) { recover_sync(); return; }   /* an image the arrays do not know: start again */
    vkResetFences(dev, 1, &fence);
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &bi);
    const VkPipelineStageFlags TOP = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, XFER = VK_PIPELINE_STAGE_TRANSFER_BIT, FRAG = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    barrier(pic, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, TOP, XFER);
    VkBufferImageCopy bc = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { (uint32_t)pic_w, (uint32_t)pic_h, 1 } };
    vkCmdCopyBufferToImage(cb, stage, pic, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    VkImage si = simg[idx];
    int want = f->grab;
    size_t need = (size_t)sext.width * sext.height * 4;
    if (want && (!rb || rb_size < need)) {
        free_buffer(&rb, &rb_mem, &rb_map);
        if (make_buffer(need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rb, &rb_mem, &rb_map)) rb_size = need; else { free_buffer(&rb, &rb_mem, &rb_map); want = 0; }
    }
    if (fsr) {
        barrier(pic, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, XFER, FRAG);
        FsrPush pc = { .in = { (float)pic_w, (float)pic_h }, .out = { (float)w, (float)h }, .origin = { (float)x0, (float)y0 },
                       .con = (float)exp2(-(1.0 - f->fsr_sharp / 100.0) * 2.0) };
        VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 }; VkRect2D sc = { { 0, 0 }, { (uint32_t)w, (uint32_t)h } };
        VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp_a, .framebuffer = ffb, .renderArea = sc };
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);   /* EASU: the picture, enlarged to the size it has in the window */
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_easu);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 1, &dset[0], 0, NULL);
        vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
        VkClearValue black = { .color = { .float32 = { 0, 0, 0, 1 } } };
        VkRect2D full = { { 0, 0 }, sext };
        VkRenderPassBeginInfo rbj = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp_b, .framebuffer = sfb[idx], .renderArea = full,
                                      .clearValueCount = 1, .pClearValues = &black };
        vkCmdBeginRenderPass(cb, &rbj, VK_SUBPASS_CONTENTS_INLINE);   /* RCAS: sharpen into the window; the rest stays black */
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_rcas);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 1, &dset[1], 0, NULL);
        vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        VkViewport vq = { (float)x0, (float)y0, (float)w, (float)h, 0, 1 }; VkRect2D sq = { { x0, y0 }, { (uint32_t)w, (uint32_t)h } };
        vkCmdSetViewport(cb, 0, 1, &vq); vkCmdSetScissor(cb, 0, 1, &sq);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);   /* the image is left in TRANSFER_SRC */
    } else {
        barrier(pic, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, XFER, XFER);
        barrier(si, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, XFER, XFER);   /* the acquire semaphore waits at XFER */
        VkClearColorValue black = { .float32 = { 0, 0, 0, 1 } };
        VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cb, si, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
        vkCmdPipelineBarrier(cb, XFER, XFER, 0, 1, &mb, 0, NULL, 0, NULL);
        VkImageBlit bl = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .srcOffsets = { { 0, 0, 0 }, { pic_w, pic_h, 1 } },
                           .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstOffsets = { { x0, y0, 0 }, { x0 + w, y0 + h, 1 } } };
        vkCmdBlitImage(cb, pic, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, si, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, f->filter && blit_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        barrier(si, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, XFER, XFER);
    }
    /* here the swapchain image is in TRANSFER_SRC either way */
    if (want) {
        VkBufferImageCopy rc = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { sext.width, sext.height, 1 } };
        vkCmdCopyImageToBuffer(cb, si, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &rc);
        VkMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
        vkCmdPipelineBarrier(cb, XFER, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);   /* a fence alone does not make device writes visible to the host */
    }
    VkAccessFlags present_access = VK_ACCESS_TRANSFER_READ_BIT;
    VkPipelineStageFlags present_stage = XFER;
    if (fsr) {
        present_access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        present_stage |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    barrier(si, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, present_access, 0, present_stage, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(cb);
    VkPipelineStageFlags wait = XFER | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &acq, .pWaitDstStageMask = &wait,
                         .commandBufferCount = 1, .pCommandBuffers = &cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &rel[idx] };
    if (vkQueueSubmit(queue, 1, &sub, fence) != VK_SUCCESS) { recover_sync(); return; }
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &rel[idx], .swapchainCount = 1, .pSwapchains = &swap, .pImageIndices = &idx };
    r = vkQueuePresentKHR(queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) sc_dirty = 1;
    if (want) {   /* --shot-window: wait for the copy, then keep the picture (0x00RRGGBB) */
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        size_t n = (size_t)sext.width * sext.height;
        uint32_t *g = realloc(grab, n * 4);
        if (g) {
            grab = g; grab_w = (int)sext.width; grab_h = (int)sext.height;
            const uint32_t *src = rb_map;
            for (size_t i = 0; i < n; i++) {
                uint32_t p = src[i];
                grab[i] = sfmt == VK_FORMAT_R8G8B8A8_UNORM ? ((p & 0xff) << 16) | (p & 0xff00) | ((p >> 16) & 0xff) : p & 0xffffff;
            }
            grab_ok = 1;
        }
    }
}
static const uint32_t *vk_grab(int *w, int *h) { if (!grab_ok) return NULL; grab_ok = 0; *w = grab_w; *h = grab_h; return grab; }   /* one-shot, like the OpenGL one */

/* the GPU hi-res renderer is not ported yet: r_init says no, so gpu_gl keeps the CPU picture */
static int vk_r_init(int scale) { (void)scale; return 0; }
static void vk_r_submit(const GPrim *p) { (void)p; }
static void vk_r_mask(int set, int check) { (void)set; (void)check; }
static void vk_r_dirty(int y0, int y1) { (void)y0; (void)y1; }
static void vk_r_flush(void) { }
static void vk_r_read(int x, int y, int w, int h, unsigned *dst) { (void)x; (void)y; (void)w; (void)h; (void)dst; }

const GfxBackend gfx_vk_backend = {
    .name = "Vulkan", .hw = 1,
    .init = vk_init, .present = vk_present, .grab = vk_grab,
    .r_init = vk_r_init, .r_submit = vk_r_submit, .r_mask = vk_r_mask,
    .r_dirty = vk_r_dirty, .r_flush = vk_r_flush, .r_read = vk_r_read
};
