/* The GPU hi-res renderer on Vulkan, steps 2 and 3 of docs/vulkan-gpu-renderer-plan.md. See gpu_vk.h.
   The vram mirror is an R8G8 image (low byte, high byte) whose changed rows are copied in through a staging buffer; the picture is a
   B8G8R8A8 image of 1024S x 512S with a depth-stencil image beside it (the stencil is the mask bit, used from step 5). Two frame
   slots, each with its own command buffer, fence, vertex buffer and staging buffer. A flush groups the queued primitives into runs
   of the same state (as gpu_gl.c does), records a draw for each with the blend pipeline and scissor it needs, and submits; when the
   vertices of a flush would not fit in a slot's buffer it submits what it has and goes on in the other slot (waiting for its fence).
   Not drawn yet: copies (GK_COPY, step 5) and the mask bit (the stencil, step 5): a primitive that sets or checks the mask is drawn
   as if the mask were off. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw.h"
#include "host.h"
#include "gpu_vk.h"
#include "gpu_emit.h"
#include "gfx_vk_spv.h"

#define QMAX 8192
#define SLOTS 2
#define VBUF_BYTES (32u << 20)
#define STAGE_BYTES (1024u * 512u * 2u)

typedef struct {
    VkCommandBuffer cb; VkFence fence; int used;
    VkBuffer vb; VkDeviceMemory vm; void *vmap;      /* vertices of this flush */
    VkBuffer sb; VkDeviceMemory sm; void *smap;      /* changed vram rows of this flush */
} Slot;
typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; } Img;
typedef struct { float target[2]; int32_t pass; float alpha; } Push;   /* what gpu_draw.vert and gpu_decode.frag read */

static int state;                               /* 0 untried, 1 ready, -1 unavailable */
static int S, tgt_scale;
static GPrim *q; static int qn;
static int cur_set, cur_chk;                    /* the mask mode, as last told to gvk_mask (used from step 5) */
static int dirty0 = 512, dirty1 = -1;
static GpuVerts gv;
static Slot slot[SLOTS]; static int cur;
static Img tgt, ds, vram_img;
static VkFormat dsfmt;
static VkRenderPass rp; static VkFramebuffer fb;
static VkSampler samp; static VkDescriptorSetLayout dsl; static VkDescriptorPool dpool; static VkDescriptorSet dset;
static VkPipelineLayout playout;
enum { BL_AVG, BL_ADD, BL_SUB, BL_QUARTER, BL_OFF, BL_N };      /* blend modes of the console (semimode 0..3) and none */
static VkPipeline pipes[BL_N];                                  /* made when first needed */
static VkShaderModule vs_mod, fs_mod;
static size_t vbuf_bytes = VBUF_BYTES;                          /* DMRECOMP_VK_VBUF_KB (a test hook) can make it smaller */
static VkCommandBuffer ucb; static VkFence ufence;                            /* reads and the first layout changes */
static VkBuffer rbuf; static VkDeviceMemory rmem; static void *rmap; static size_t rsize;   /* the read-back */
static int ignored;                             /* copies seen so far (said once: they are not drawn yet) */
typedef struct { int first, count, blend, clip[4]; int32_t pass; float alpha; } Draw;   /* a range of the batch's vertices and its state */
static Draw *dr; static int nd, dcap;

static void free_img(Img *i) {
    if (i->view) vkDestroyImageView(vkc.dev, i->view, NULL);
    if (i->img) vkDestroyImage(vkc.dev, i->img, NULL);
    if (i->mem) vkFreeMemory(vkc.dev, i->mem, NULL);
    memset(i, 0, sizeof *i);
}
static int make_img(Img *i, int w, int h, VkFormat fmt, VkImageUsageFlags use, VkImageAspectFlags aspect) {
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { (uint32_t)w, (uint32_t)h, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = use,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (vkCreateImage(vkc.dev, &ii, NULL, &i->img) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(vkc.dev, i->img, &mr);
    int t = vk_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t < 0) t = vk_find_mem(mr.memoryTypeBits, 0);
    if (t < 0) return 0;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)t };
    if (vkAllocateMemory(vkc.dev, &ai, NULL, &i->mem) != VK_SUCCESS || vkBindImageMemory(vkc.dev, i->img, i->mem, 0) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = i->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { aspect, 0, 1, 0, 1 } };
    return vkCreateImageView(vkc.dev, &vi, NULL, &i->view) == VK_SUCCESS;
}
static void layout(VkCommandBuffer c, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da,
                   VkPipelineStageFlags ss, VkPipelineStageFlags ds_) {
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = { aspect, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(c, ss, ds_, 0, 0, NULL, 0, NULL, 1, &b);
}
static int run_once(VkCommandBuffer c) {   /* submit c and wait for it */
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &c };
    vkResetFences(vkc.dev, 1, &ufence);
    if (vkQueueSubmit(vkc.queue, 1, &si, ufence) != VK_SUCCESS) return 0;
    return vkWaitForFences(vkc.dev, 1, &ufence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
}

/* ---- things made once ---- */
static VkFormat pick_depth_stencil(void) {
    static const VkFormat want[2] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };
    for (int i = 0; i < 2; i++) {
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(vkc.phys, want[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) return want[i];   /* only drawn into and tested, never read */
    }
    return VK_FORMAT_UNDEFINED;
}
static int make_pass(void) {
    VkAttachmentDescription at[2] = {
        { .format = VK_FORMAT_B8G8R8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
        { .format = dsfmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
          .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL } };
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, dref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &cref, .pDepthStencilAttachment = &dref };
    const VkPipelineStageFlags att = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags rw = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkSubpassDependency dep[2] = {
        { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0, .srcStageMask = att, .srcAccessMask = rw, .dstStageMask = att, .dstAccessMask = rw },
        { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL, .srcStageMask = att, .srcAccessMask = rw,
          .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT } };
    VkRenderPassCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2, .pAttachments = at, .subpassCount = 1, .pSubpasses = &sp,
        .dependencyCount = 2, .pDependencies = dep };
    return vkCreateRenderPass(vkc.dev, &ri, NULL, &rp) == VK_SUCCESS;
}
static VkPipeline make_pipeline(int bl) {
    VkPipeline out = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs_mod, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs_mod, .pName = "main" } };
    VkVertexInputBindingDescription bd = { 0, sizeof(GlsVert), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription ad[5] = {
        { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 8 }, { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 20 },
        { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 36 }, { 4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 52 } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &bd, .vertexAttributeDescriptionCount = 5, .pVertexAttributeDescriptions = ad };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo dss = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };   /* depth and stencil tests off */
    /* the blend as the OpenGL renderer sets it (set_blend in gpu_gl.c): the same factors for colour and alpha */
    VkBlendFactor src = VK_BLEND_FACTOR_ONE, dstf = VK_BLEND_FACTOR_ONE; VkBlendOp op = VK_BLEND_OP_ADD;
    if (bl == BL_AVG) { src = VK_BLEND_FACTOR_SRC_ALPHA; dstf = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; }   /* B/2 + F/2 */
    else if (bl == BL_QUARTER) src = VK_BLEND_FACTOR_SRC_ALPHA;                                         /* B + F/4 */
    else if (bl == BL_SUB) op = VK_BLEND_OP_REVERSE_SUBTRACT;                                           /* B - F */
    VkPipelineColorBlendAttachmentState ba = { .blendEnable = bl != BL_OFF, .srcColorBlendFactor = src, .dstColorBlendFactor = dstf, .colorBlendOp = op,
        .srcAlphaBlendFactor = src, .dstAlphaBlendFactor = dstf, .alphaBlendOp = op,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
    VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds_ = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &dss,
        .pColorBlendState = &cbs, .pDynamicState = &ds_, .layout = playout, .renderPass = rp, .subpass = 0 };
    if (vkCreateGraphicsPipelines(vkc.dev, VK_NULL_HANDLE, 1, &gi, NULL, &out) != VK_SUCCESS) out = VK_NULL_HANDLE;
    return out;
}
static VkPipeline get_pipe(int bl) {
    if (!pipes[bl]) pipes[bl] = make_pipeline(bl);
    return pipes[bl];
}
static int setup_once(void) {
    const char *kb = getenv("DMRECOMP_VK_VBUF_KB");   /* a test hook: a small vertex buffer makes a flush split into several submits */
    if (kb && atoi(kb) >= 4) vbuf_bytes = (size_t)atoi(kb) * 1024;
    if (vbuf_bytes > VBUF_BYTES) vbuf_bytes = VBUF_BYTES;
    dsfmt = pick_depth_stencil();
    if (dsfmt == VK_FORMAT_UNDEFINED) { fprintf(stderr, "[gpu_vk] this device offers no depth-stencil format the picture can use; using the CPU picture\n"); return -1; }
    VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    VkDescriptorSetLayoutBinding lb = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &lb };
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    if (vkCreateSampler(vkc.dev, &si, NULL, &samp) != VK_SUCCESS || vkCreateDescriptorSetLayout(vkc.dev, &li, NULL, &dsl) != VK_SUCCESS
        || vkCreateDescriptorPool(vkc.dev, &pi, NULL, &dpool) != VK_SUCCESS) return -1;
    VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    if (vkAllocateDescriptorSets(vkc.dev, &ai, &dset) != VK_SUCCESS) return -1;
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push) };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    if (vkCreatePipelineLayout(vkc.dev, &pli, NULL, &playout) != VK_SUCCESS || !make_pass()) return -1;
    vs_mod = vk_make_module(spv_gpu_draw_vert, sizeof spv_gpu_draw_vert / 4); fs_mod = vk_make_module(spv_gpu_decode_frag, sizeof spv_gpu_decode_frag / 4);
    if (!vs_mod || !fs_mod || !get_pipe(BL_OFF)) return -1;
    /* command buffers, fences, per-slot buffers */
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkCommandBufferAllocateInfo ba = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = vkc.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    if (vkAllocateCommandBuffers(vkc.dev, &ba, &ucb) != VK_SUCCESS || vkCreateFence(vkc.dev, &fi, NULL, &ufence) != VK_SUCCESS) return -1;
    for (int i = 0; i < SLOTS; i++) {
        if (vkAllocateCommandBuffers(vkc.dev, &ba, &slot[i].cb) != VK_SUCCESS || vkCreateFence(vkc.dev, &fi, NULL, &slot[i].fence) != VK_SUCCESS
            || !vk_make_buffer(vbuf_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &slot[i].vb, &slot[i].vm, &slot[i].vmap)
            || !vk_make_buffer(STAGE_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &slot[i].sb, &slot[i].sm, &slot[i].smap)) return -1;
    }
    /* the vram mirror, 1024x512, two bytes a texel */
    if (!make_img(&vram_img, 1024, 512, VK_FORMAT_R8G8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) return -1;
    VkDescriptorImageInfo di = { .sampler = samp, .imageView = vram_img.view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &di };
    vkUpdateDescriptorSets(vkc.dev, 1, &wr, 0, NULL);
    q = malloc(sizeof *q * QMAX);
    if (!q) return -1;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkResetCommandBuffer(ucb, 0); vkBeginCommandBuffer(ucb, &bi);
    layout(ucb, vram_img.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT,
           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    vkEndCommandBuffer(ucb);
    return run_once(ucb) ? 1 : -1;
}
/* the picture and its stencil at the scale asked for */
static void free_target(void) {
    if (fb) vkDestroyFramebuffer(vkc.dev, fb, NULL);
    fb = VK_NULL_HANDLE;
    free_img(&tgt); free_img(&ds);
    tgt_scale = 0;
}
static int make_target(int scale) {
    VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(vkc.phys, &pr);
    uint32_t w = 1024u * scale, h = 512u * scale;
    if (w > pr.limits.maxImageDimension2D || w > pr.limits.maxFramebufferWidth || h > pr.limits.maxFramebufferHeight) {
        fprintf(stderr, "[gpu_vk] the device cannot make a %u pixel wide picture; using the CPU picture\n", w);
        return 0;
    }
    vkDeviceWaitIdle(vkc.dev);
    free_target();
    if (!make_img(&tgt, (int)w, (int)h, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT)
        || !make_img(&ds, (int)w, (int)h, dsfmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) { free_target(); return 0; }
    VkImageView views[2] = { tgt.view, ds.view };
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 2, .pAttachments = views, .width = w, .height = h, .layers = 1 };
    if (vkCreateFramebuffer(vkc.dev, &fci, NULL, &fb) != VK_SUCCESS) { free_target(); return 0; }
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkResetCommandBuffer(ucb, 0); vkBeginCommandBuffer(ucb, &bi);   /* both images start in the layouts the render pass expects */
    layout(ucb, tgt.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
           VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    layout(ucb, ds.img, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
    vkEndCommandBuffer(ucb);
    if (!run_once(ucb)) { free_target(); return 0; }
    tgt_scale = scale;
    return 1;
}

int gvk_init(int scale) {
    if (state < 0 || !vkc.dev) return 0;
    if (state == 0) {
        const char *on = getenv("DMRECOMP_VK_RENDERER");   /* step 2 of the plan: not for the game yet */
        if (!on || !*on || !strcmp(on, "0")) { state = -1; return 0; }
        state = setup_once();
        if (state < 0) {
            fprintf(stderr, "[gpu_vk] the renderer could not be set up; using the CPU picture\n");
            gvk_shutdown();   /* what was made before the failure goes now, not at the end of the session */
            state = -1;
            return 0;
        }
    }
    if (tgt_scale != scale && !make_target(scale)) return 0;   /* the renderer stays usable: a later scale may work */
    S = scale; qn = 0; cur_chk = 0; cur_set = 0;
    dirty0 = 0; dirty1 = 511;
    fprintf(stderr, "[gpu_vk] drawing the %dx picture on the graphics card (depth-stencil %s; copies and the mask bit are not drawn yet)\n", scale,
            dsfmt == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8");
    return 1;
}

void gvk_dirty(int y0, int y1) {
    if (y0 < 0 || y1 > 511 || y1 - y0 >= 511) { dirty0 = 0; dirty1 = 511; return; }
    if (y0 < dirty0) dirty0 = y0;
    if (y1 > dirty1) dirty1 = y1;
}
void gvk_mask(int set, int check) { cur_set = set != 0; cur_chk = check != 0; }

/* the drawing area of a primitive, inclusive, in vram pixels; fills and blits are not clipped */
static void cliprect(const GPrim *p, int c[4]) {
    if (p->kind == GK_TRI || p->kind == GK_RECT) memcpy(c, p->clip, 4 * sizeof(int));
    else { c[0] = 0; c[1] = 0; c[2] = 1023; c[3] = 511; }
}
/* may b be drawn in the same run as a: the same drawing area, blend and kind of draw (as gpu_gl.c groups them) */
static int same_state(const GPrim *a, const GPrim *b) {
    if (a->chkm || b->chkm) return 0;   /* with the mask checked every prim is on its own: the stencil depends on drawing order */
    if (a->kind == GK_COPY || b->kind == GK_COPY || (a->semi && a->textured) || (b->semi && b->textured)) return 0;   /* translucent textured ones are drawn one at a time, in two passes */
    int ca[4], cb[4]; cliprect(a, ca); cliprect(b, cb);
    return memcmp(ca, cb, sizeof ca) == 0 && a->semi == b->semi && a->semimode == b->semimode && a->setm == b->setm && a->chkm == b->chkm
        && (a->kind == GK_FILL) == (b->kind == GK_FILL) && (a->kind == GK_BLIT) == (b->kind == GK_BLIT);
}
static int blend_of(int semimode) { return semimode == 0 ? BL_AVG : semimode == 3 ? BL_QUARTER : semimode == 2 ? BL_SUB : BL_ADD; }
static void add_draw(const Draw *d) {
    if (nd == dcap) {
        Draw *nb = realloc(dr, sizeof *dr * (size_t)(dcap ? dcap * 2 : 256));
        if (!nb) return;   /* out of memory: this draw is lost */
        dr = nb; dcap = dcap ? dcap * 2 : 256;
    }
    dr[nd++] = *d;
}
/* the vertices first .. first + count of the batch came from the run whose first primitive is f */
static void add_run(const GPrim *f, int first, int count) {
    if (count <= 0) return;
    Draw d = { .first = first, .count = count, .blend = BL_OFF, .alpha = 1.0f };
    cliprect(f, d.clip);
    if (d.clip[2] < d.clip[0] || d.clip[3] < d.clip[1]) return;   /* an empty drawing area */
    if (f->semi && f->textured) {   /* opaque texels first, then the translucent ones */
        d.pass = 1; add_draw(&d);
        d.pass = 2; d.blend = blend_of(f->semimode); d.alpha = f->semimode == 0 ? 0.5f : f->semimode == 3 ? 0.25f : 1.0f; add_draw(&d);
    } else {
        if (f->semi) { d.blend = blend_of(f->semimode); d.alpha = f->semimode == 0 ? 0.5f : f->semimode == 3 ? 0.25f : 1.0f; }
        add_draw(&d);
    }
}
/* record the dirty vram rows and the draws of the batch into the current slot, submit it, and move on to the other slot */
static void submit_batch(void) {
    int upload = dirty1 >= dirty0;
    if (gv.fail) { fprintf(stderr, "[gpu_vk] out of memory for the vertices; part of a frame is not drawn\n"); gv.fail = 0; gv.n = 0; nd = 0; }
    if (!nd && !upload) { gv.n = 0; return; }
    Slot *sl = &slot[cur];
    if (sl->used) { vkWaitForFences(vkc.dev, 1, &sl->fence, VK_TRUE, UINT64_MAX); sl->used = 0; }
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkResetCommandBuffer(sl->cb, 0); vkBeginCommandBuffer(sl->cb, &bi);
    if (upload) {   /* the rows of vram the CPU changed since the last flush */
        uint32_t rows = (uint32_t)(dirty1 - dirty0 + 1);
        memcpy(sl->smap, &vram[dirty0 * 1024], (size_t)rows * 1024 * 2);
        layout(sl->cb, vram_img.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy bc = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageOffset = { 0, dirty0, 0 }, .imageExtent = { 1024, rows, 1 } };
        vkCmdCopyBufferToImage(sl->cb, sl->sb, vram_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
        layout(sl->cb, vram_img.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    if (nd) {
        memcpy(sl->vmap, gv.v, (size_t)gv.n * sizeof(GlsVert));
        uint32_t w = 1024u * S, h = 512u * S;
        VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = { { 0, 0 }, { w, h } } };
        vkCmdBeginRenderPass(sl->cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindDescriptorSets(sl->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 1, &dset, 0, NULL);
        VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
        vkCmdSetViewport(sl->cb, 0, 1, &vp);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(sl->cb, 0, 1, &sl->vb, &off);
        VkPipeline bound = VK_NULL_HANDLE;
        for (int i = 0; i < nd; i++) {
            const Draw *d = &dr[i];
            VkPipeline pl = get_pipe(d->blend);
            if (!pl) { fprintf(stderr, "[gpu_vk] a blend pipeline could not be made; a draw is skipped\n"); continue; }
            if (pl != bound) { vkCmdBindPipeline(sl->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl); bound = pl; }
            VkRect2D sc = { { 0, 0 }, { w, h } };
            if (!(d->clip[0] == 0 && d->clip[1] == 0 && d->clip[2] == 1023 && d->clip[3] == 511))
                sc = (VkRect2D){ { d->clip[0] * S, d->clip[1] * S }, { (uint32_t)((d->clip[2] - d->clip[0] + 1) * S), (uint32_t)((d->clip[3] - d->clip[1] + 1) * S) } };
            vkCmdSetScissor(sl->cb, 0, 1, &sc);
            Push pc = { { (float)w, (float)h }, d->pass, d->alpha };
            vkCmdPushConstants(sl->cb, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
            vkCmdDraw(sl->cb, (uint32_t)d->count, 1, (uint32_t)d->first, 0);
        }
        vkCmdEndRenderPass(sl->cb);
    }
    vkEndCommandBuffer(sl->cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &sl->cb };
    vkResetFences(vkc.dev, 1, &sl->fence);
    if (vkQueueSubmit(vkc.queue, 1, &si, sl->fence) == VK_SUCCESS) {
        sl->used = 1; cur = (cur + 1) % SLOTS;
        if (upload) { dirty0 = 512; dirty1 = -1; }   /* only now: after a failed submit the rows are still to be sent */
    } else {
        fprintf(stderr, "[gpu_vk] a flush could not be submitted\n");
        vkDeviceWaitIdle(vkc.dev);
        VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };   /* the fence was reset and nothing will signal it */
        vkDestroyFence(vkc.dev, sl->fence, NULL); sl->fence = VK_NULL_HANDLE;
        vkCreateFence(vkc.dev, &fi, NULL, &sl->fence);
    }
    gv.n = 0; nd = 0;
}

void gvk_flush(void) {
    if (state <= 0 || !qn) return;
    gv.n = 0; gv.fail = 0; nd = 0;
    const int cap = (int)(vbuf_bytes / sizeof(GlsVert));
    int i = 0;
    while (i < qn) {
        const GPrim *f = &q[i];
        if (f->kind == GK_COPY) { if (!ignored++) fprintf(stderr, "[gpu_vk] copies are not drawn yet (step 5)\n"); i++; continue; }
        int j = i + 1;
        while (j < qn && same_state(f, &q[j])) j++;
        int start = gv.n;
        for (int k = i; k < j; k++) {
            if (gv.n + 24 > cap) { add_run(f, start, gv.n - start); submit_batch(); start = 0; }   /* the slot's buffer is full: submit, go on in the other slot */
            gv_emit(&gv, &q[k], S);
        }
        add_run(f, start, gv.n - start);
        i = j;
    }
    submit_batch();
    qn = 0;
}

void gvk_submit(const GPrim *p) {
    if (state <= 0) return;
    if (qn == QMAX) gvk_flush();
    q[qn] = *p; q[qn].setm = cur_set; q[qn].chkm = cur_chk;
    qn++;
}

/* ---- display: the visible box wraps around the edges of vram like the CPU picture does (gv_runs, gpu_emit.h) ---- */
void gvk_present_copy(VkCommandBuffer c, VkImage dst, int x, int y, int w, int h) {
    if (state <= 0 || w <= 0 || h <= 0) return;
    if (w > 1024) w = 1024;
    if (h > 512) h = 512;
    int rx[2][3], ry[2][3], nx = gv_runs(x, w, 1024, rx), ny = gv_runs(y, h, 512, ry);
    layout(c, tgt.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy rg[4]; int n = 0;
    for (int j = 0; j < ny; j++) for (int i = 0; i < nx; i++) {
        rg[n++] = (VkImageCopy){ .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .srcOffset = { rx[i][0] * S, ry[j][0] * S, 0 },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstOffset = { rx[i][1] * S, ry[j][1] * S, 0 },
            .extent = { (uint32_t)(rx[i][2] * S), (uint32_t)(ry[j][2] * S), 1 } };
    }
    vkCmdCopyImage(c, tgt.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)n, rg);
    layout(c, tgt.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
           VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
}

void gvk_read(int x, int y, int w, int h, unsigned *dst) {
    if (state <= 0 || w <= 0 || h <= 0) return;
    if (w > 1024) w = 1024;
    if (h > 512) h = 512;
    gvk_flush();
    vkDeviceWaitIdle(vkc.dev);
    size_t need = (size_t)w * S * h * S * 4;
    if (rsize < need) {
        vk_free_buffer(&rbuf, &rmem, &rmap);
        if (!vk_make_buffer(need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rbuf, &rmem, &rmap)) { vk_free_buffer(&rbuf, &rmem, &rmap); rsize = 0; return; }
        rsize = need;
    }
    int rx[2][3], ry[2][3], nx = gv_runs(x, w, 1024, rx), ny = gv_runs(y, h, 512, ry);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkResetCommandBuffer(ucb, 0); vkBeginCommandBuffer(ucb, &bi);
    layout(ucb, tgt.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy rg[4]; int n = 0;
    for (int j = 0; j < ny; j++) for (int i = 0; i < nx; i++) {   /* the pieces land in one picture w * S wide */
        rg[n++] = (VkBufferImageCopy){ .bufferOffset = ((VkDeviceSize)ry[j][1] * S * (VkDeviceSize)(w * S) + (VkDeviceSize)rx[i][1] * S) * 4, .bufferRowLength = (uint32_t)(w * S),
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageOffset = { rx[i][0] * S, ry[j][0] * S, 0 },
            .imageExtent = { (uint32_t)(rx[i][2] * S), (uint32_t)(ry[j][2] * S), 1 } };
    }
    vkCmdCopyImageToBuffer(ucb, tgt.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, (uint32_t)n, rg);
    layout(ucb, tgt.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
           VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(ucb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(ucb);
    if (run_once(ucb)) memcpy(dst, rmap, need);
}

void gvk_shutdown(void) {
    if (!vkc.dev) return;
    vkDeviceWaitIdle(vkc.dev);
    free_target(); free_img(&vram_img);
    vk_free_buffer(&rbuf, &rmem, &rmap); rsize = 0;
    for (int i = 0; i < SLOTS; i++) {
        vk_free_buffer(&slot[i].vb, &slot[i].vm, &slot[i].vmap); vk_free_buffer(&slot[i].sb, &slot[i].sm, &slot[i].smap);
        if (slot[i].fence) vkDestroyFence(vkc.dev, slot[i].fence, NULL);
        memset(&slot[i], 0, sizeof slot[i]);   /* the command buffers go with the pool */
    }
    if (ufence) vkDestroyFence(vkc.dev, ufence, NULL);
    for (int i = 0; i < BL_N; i++) { if (pipes[i]) vkDestroyPipeline(vkc.dev, pipes[i], NULL); pipes[i] = VK_NULL_HANDLE; }
    if (vs_mod) vkDestroyShaderModule(vkc.dev, vs_mod, NULL);
    if (fs_mod) vkDestroyShaderModule(vkc.dev, fs_mod, NULL);
    vs_mod = fs_mod = VK_NULL_HANDLE;
    if (playout) vkDestroyPipelineLayout(vkc.dev, playout, NULL);
    if (dpool) vkDestroyDescriptorPool(vkc.dev, dpool, NULL);
    if (dsl) vkDestroyDescriptorSetLayout(vkc.dev, dsl, NULL);
    if (samp) vkDestroySampler(vkc.dev, samp, NULL);
    if (rp) vkDestroyRenderPass(vkc.dev, rp, NULL);
    ufence = VK_NULL_HANDLE; playout = VK_NULL_HANDLE; dpool = VK_NULL_HANDLE; dsl = VK_NULL_HANDLE; samp = VK_NULL_HANDLE; rp = VK_NULL_HANDLE;
    free(q); q = NULL; free(gv.v); memset(&gv, 0, sizeof gv); free(dr); dr = NULL; nd = dcap = 0;
    state = 0; qn = 0; cur = 0;
}
