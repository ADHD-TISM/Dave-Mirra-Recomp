/* The Vulkan device and what every Vulkan file shares: the entry points (looked up at run time, nothing links against Vulkan), the
   instance, device, queue and command pool, and a few helpers. gfx_vk.c makes them; the GPU renderer
   (docs/vulkan-gpu-renderer-plan.md) uses them from its own file. */
#ifndef VK_CTX_H
#define VK_CTX_H
#define VK_NO_PROTOTYPES
#include <stddef.h>
#include "vulkan/vulkan.h"

/* every device and instance function the Vulkan backend and the GPU renderer call; loaded together, so one that is missing fails the
   backend's init (all are Vulkan 1.0 core, apart from the swapchain and surface ones) */
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
    X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdCopyImage) X(vkCmdBindVertexBuffers) X(vkCmdSetStencilReference) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask)
#define X(n) extern PFN_##n n;
VK_FUNCS(X)
#undef X

typedef struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;                          /* the one graphics queue, which can also present */
    uint32_t qfam;                          /* its family */
    VkPhysicalDeviceMemoryProperties mem;
    VkCommandPool pool;                     /* command buffers made from it can be reset one by one */
} VkCtx;
extern VkCtx vkc;                           /* filled in by the Vulkan backend's init; all zero when it is not running */

int  vk_find_mem(uint32_t type_bits, VkMemoryPropertyFlags want);   /* a memory type index, or -1 */
int  vk_make_buffer(size_t size, VkBufferUsageFlags use, VkBuffer *b, VkDeviceMemory *m, void **map);   /* host-visible, coherent, mapped; cached too when use has TRANSFER_DST, which means the CPU reads it back */
void vk_free_buffer(VkBuffer *b, VkDeviceMemory *m, void **map);
VkShaderModule vk_make_module(const uint32_t *code, size_t words);   /* VK_NULL_HANDLE on failure */
void vk_image_barrier(VkCommandBuffer c, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da,
                      VkPipelineStageFlags ss, VkPipelineStageFlags ds);   /* one colour mip, one layer */
#endif
