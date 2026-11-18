// The player menu's triangles drawn with Vulkan (port/platform/present_vk_ui.hpp).
//
// One pipeline: RmlUi's vertices in the window's pixels, moved by each draw's translation (a push constant), the
// vertex colour times a texture (a white pixel for draws without one), blended premultiplied over the picture.
// Each texture is a device image with its own descriptor set; a texture RmlUi lets go of is freed once every frame
// in flight that could draw it has finished (FRAMES prepares later). Each frame slot has its own host-visible vertex
// and index buffers, grown as needed, and its own staging buffers, freed when the slot comes round again.
#include "present_vk_ui.hpp"

#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "../ui/shaders/ui_spirv.h"

namespace {

#define UI_DEVICE_FUNCTIONS(X)                                                                                         \
    X(vkCreateBuffer)                                                                                                  \
    X(vkDestroyBuffer)                                                                                                 \
    X(vkCreateImage)                                                                                                   \
    X(vkDestroyImage)                                                                                                  \
    X(vkCreateImageView)                                                                                               \
    X(vkDestroyImageView)                                                                                              \
    X(vkCreateSampler)                                                                                                 \
    X(vkDestroySampler)                                                                                                \
    X(vkAllocateMemory)                                                                                                \
    X(vkFreeMemory)                                                                                                    \
    X(vkBindBufferMemory)                                                                                              \
    X(vkBindImageMemory)                                                                                               \
    X(vkGetBufferMemoryRequirements)                                                                                   \
    X(vkGetImageMemoryRequirements)                                                                                    \
    X(vkMapMemory)                                                                                                     \
    X(vkCreateShaderModule)                                                                                            \
    X(vkDestroyShaderModule)                                                                                           \
    X(vkCreateDescriptorSetLayout)                                                                                     \
    X(vkDestroyDescriptorSetLayout)                                                                                    \
    X(vkCreateDescriptorPool)                                                                                          \
    X(vkDestroyDescriptorPool)                                                                                         \
    X(vkAllocateDescriptorSets)                                                                                        \
    X(vkFreeDescriptorSets)                                                                                            \
    X(vkUpdateDescriptorSets)                                                                                          \
    X(vkCreatePipelineLayout)                                                                                          \
    X(vkDestroyPipelineLayout)                                                                                         \
    X(vkCreateGraphicsPipelines)                                                                                       \
    X(vkDestroyPipeline)                                                                                               \
    X(vkCmdPipelineBarrier)                                                                                            \
    X(vkCmdCopyBufferToImage)                                                                                          \
    X(vkCmdBindPipeline)                                                                                               \
    X(vkCmdBindDescriptorSets)                                                                                         \
    X(vkCmdBindVertexBuffers)                                                                                          \
    X(vkCmdBindIndexBuffer)                                                                                            \
    X(vkCmdSetViewport)                                                                                                \
    X(vkCmdSetScissor)                                                                                                 \
    X(vkCmdPushConstants)                                                                                              \
    X(vkCmdDrawIndexed)

#define DECLARE(name) PFN_##name name = nullptr;
struct Functions {
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
    UI_DEVICE_FUNCTIONS(DECLARE)
};
#undef DECLARE

const uint32_t MAX_SLOTS = 4;
const uint32_t MAX_TEXTURES = 512; // descriptor sets: RmlUi's font pages and the white pixel

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *map = nullptr;
    VkDeviceSize size = 0;
};

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
};

struct Slot {
    Buffer vertices;
    Buffer indices;
    std::vector<Buffer> staging; // this slot's uploads, freed when it comes round again
};

struct Release {
    ui::TextureId id;
    uint64_t when; // freed at this prepare count, when no frame in flight can draw it
};

struct Drawer {
    bool up = false;
    Functions vk;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_types = {};
    uint32_t frames = 0;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    Texture white;
    bool white_uploaded = false;
    std::unordered_map<ui::TextureId, Texture> textures;
    std::vector<Release> releases;
    uint64_t prepared = 0;
    Slot slots[MAX_SLOTS];
};
Drawer d;

struct Push {
    float scale[2];
    float translation[2];
};

int memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < d.memory_types.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (d.memory_types.memoryTypes[i].propertyFlags & want) == want) {
            return int(i);
        }
    }
    return -1;
}

void free_buffer(Buffer &b) {
    if (b.buffer != VK_NULL_HANDLE) {
        d.vk.vkDestroyBuffer(d.device, b.buffer, nullptr);
    }
    if (b.memory != VK_NULL_HANDLE) {
        d.vk.vkFreeMemory(d.device, b.memory, nullptr); // (unmapped with it)
    }
    b = Buffer();
}

bool make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer &out) {
    VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (d.vk.vkCreateBuffer(d.device, &info, nullptr, &out.buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req;
    d.vk.vkGetBufferMemoryRequirements(d.device, out.buffer, &req);
    int type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = uint32_t(type);
    if (type < 0 || d.vk.vkAllocateMemory(d.device, &alloc, nullptr, &out.memory) != VK_SUCCESS ||
        d.vk.vkBindBufferMemory(d.device, out.buffer, out.memory, 0) != VK_SUCCESS ||
        d.vk.vkMapMemory(d.device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.map) != VK_SUCCESS) {
        free_buffer(out);
        return false;
    }
    out.size = size;
    return true;
}

// A buffer of at least size bytes (grown by half again, so a menu that grows a little does not make one each frame)
bool fit_buffer(Buffer &b, VkDeviceSize size, VkBufferUsageFlags usage) {
    if (b.size >= size) {
        return true;
    }
    free_buffer(b);
    return make_buffer(size + size / 2, usage, b);
}

void free_texture(Texture &t) {
    if (t.set != VK_NULL_HANDLE) {
        d.vk.vkFreeDescriptorSets(d.device, d.pool, 1, &t.set);
    }
    if (t.view != VK_NULL_HANDLE) {
        d.vk.vkDestroyImageView(d.device, t.view, nullptr);
    }
    if (t.image != VK_NULL_HANDLE) {
        d.vk.vkDestroyImage(d.device, t.image, nullptr);
    }
    if (t.memory != VK_NULL_HANDLE) {
        d.vk.vkFreeMemory(d.device, t.memory, nullptr);
    }
    t = Texture();
}

bool make_texture(uint32_t width, uint32_t height, Texture &out) {
    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (d.vk.vkCreateImage(d.device, &info, nullptr, &out.image) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req;
    d.vk.vkGetImageMemoryRequirements(d.device, out.image, &req);
    int type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = uint32_t(type);
    if (type < 0 || d.vk.vkAllocateMemory(d.device, &alloc, nullptr, &out.memory) != VK_SUCCESS ||
        d.vk.vkBindImageMemory(d.device, out.image, out.memory, 0) != VK_SUCCESS) {
        free_texture(out);
        return false;
    }
    VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = out.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = info.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDescriptorSetAllocateInfo set = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set.descriptorPool = d.pool;
    set.descriptorSetCount = 1;
    set.pSetLayouts = &d.set_layout;
    if (d.vk.vkCreateImageView(d.device, &view, nullptr, &out.view) != VK_SUCCESS ||
        d.vk.vkAllocateDescriptorSets(d.device, &set, &out.set) != VK_SUCCESS) {
        free_texture(out);
        return false;
    }
    VkDescriptorImageInfo image = {d.sampler, out.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = out.set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    d.vk.vkUpdateDescriptorSets(d.device, 1, &write, 0, nullptr);
    return true;
}

void layout_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags src,
                    VkAccessFlags src_access, VkPipelineStageFlags dst, VkAccessFlags dst_access) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    d.vk.vkCmdPipelineBarrier(cmd, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// The pixels into the texture through a staging buffer of the slot's, in cmd
bool upload(VkCommandBuffer cmd, Slot &slot, const Texture &t, uint32_t width, uint32_t height, const uint8_t *rgba) {
    Buffer staging;
    VkDeviceSize size = VkDeviceSize(width) * height * 4;
    if (!make_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging)) {
        return false;
    }
    std::memcpy(staging.map, rgba, size_t(size));
    slot.staging.push_back(staging);
    layout_barrier(cmd, t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy = {};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    d.vk.vkCmdCopyBufferToImage(cmd, staging.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    layout_barrier(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_SHADER_READ_BIT);
    return true;
}

VkShaderModule make_shader(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes;
    info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    return d.vk.vkCreateShaderModule(d.device, &info, nullptr, &module) == VK_SUCCESS ? module : VK_NULL_HANDLE;
}

bool make_pipeline(VkRenderPass pass) {
    VkSamplerCreateInfo sampler = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 0.0f;
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                            nullptr};
    VkDescriptorSetLayoutCreateInfo set_layout = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_layout.bindingCount = 1;
    set_layout.pBindings = &binding;
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEXTURES};
    VkDescriptorPoolCreateInfo pool = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool.maxSets = MAX_TEXTURES;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &pool_size;
    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push)};
    if (d.vk.vkCreateSampler(d.device, &sampler, nullptr, &d.sampler) != VK_SUCCESS ||
        d.vk.vkCreateDescriptorSetLayout(d.device, &set_layout, nullptr, &d.set_layout) != VK_SUCCESS ||
        d.vk.vkCreateDescriptorPool(d.device, &pool, nullptr, &d.pool) != VK_SUCCESS) {
        return false;
    }
    VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &d.set_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (d.vk.vkCreatePipelineLayout(d.device, &layout, nullptr, &d.layout) != VK_SUCCESS) {
        return false;
    }

    VkShaderModule vert = make_shader(ui_vert_spirv, sizeof(ui_vert_spirv));
    VkShaderModule frag = make_shader(ui_frag_spirv, sizeof(ui_frag_spirv));
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    VkVertexInputBindingDescription vertex = {0, sizeof(ui::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[3] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(ui::Vertex, x))},
        {1, 0, VK_FORMAT_R8G8B8A8_UNORM, uint32_t(offsetof(ui::Vertex, r))},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(ui::Vertex, u))},
    };
    VkPipelineVertexInputStateCreateInfo input = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    input.vertexBindingDescriptionCount = 1;
    input.pVertexBindingDescriptions = &vertex;
    input.vertexAttributeDescriptionCount = 3;
    input.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend = {};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; // (premultiplied)
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blending = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;
    VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blending;
    info.pDynamicState = &dynamic;
    info.layout = d.layout;
    info.renderPass = pass;
    info.subpass = 0;
    bool made = vert != VK_NULL_HANDLE && frag != VK_NULL_HANDLE &&
                d.vk.vkCreateGraphicsPipelines(d.device, VK_NULL_HANDLE, 1, &info, nullptr, &d.pipeline) == VK_SUCCESS;
    if (vert != VK_NULL_HANDLE) {
        d.vk.vkDestroyShaderModule(d.device, vert, nullptr);
    }
    if (frag != VK_NULL_HANDLE) {
        d.vk.vkDestroyShaderModule(d.device, frag, nullptr);
    }
    return made;
}

} // namespace

bool ui_vk_open(const UiVkSetup &setup) {
    ui_vk_close();
    if (setup.frames == 0 || setup.frames > MAX_SLOTS) {
        return false;
    }
    d.device = setup.device;
    d.frames = setup.frames;
    auto get_device = PFN_vkGetDeviceProcAddr(setup.get_instance_proc_addr(setup.instance, "vkGetDeviceProcAddr"));
    d.vk.vkGetPhysicalDeviceMemoryProperties = PFN_vkGetPhysicalDeviceMemoryProperties(
        setup.get_instance_proc_addr(setup.instance, "vkGetPhysicalDeviceMemoryProperties"));
    bool loaded = get_device != nullptr && d.vk.vkGetPhysicalDeviceMemoryProperties != nullptr;
#define LOAD(name)                                                                                                     \
    if (loaded) {                                                                                                      \
        d.vk.name = PFN_##name(get_device(d.device, #name));                                                           \
        loaded = d.vk.name != nullptr;                                                                                 \
    }
    UI_DEVICE_FUNCTIONS(LOAD)
#undef LOAD
    if (!loaded) {
        std::fprintf(stderr, "[ui] Vulkan's functions could not be loaded: the menu is not drawn\n");
        d = Drawer();
        return false;
    }
    d.vk.vkGetPhysicalDeviceMemoryProperties(setup.gpu, &d.memory_types);
    d.up = true; // (from here ui_vk_close frees what was made)
    if (!make_pipeline(setup.pass) || !make_texture(1, 1, d.white)) {
        std::fprintf(stderr, "[ui] the menu's pipeline could not be made: it is not drawn\n");
        ui_vk_close();
        return false;
    }
    return true;
}

void ui_vk_close() {
    if (!d.up) {
        return;
    }
    for (Slot &slot : d.slots) {
        free_buffer(slot.vertices);
        free_buffer(slot.indices);
        for (Buffer &b : slot.staging) {
            free_buffer(b);
        }
    }
    for (auto &entry : d.textures) {
        free_texture(entry.second);
    }
    free_texture(d.white);
    if (d.pipeline != VK_NULL_HANDLE) {
        d.vk.vkDestroyPipeline(d.device, d.pipeline, nullptr);
    }
    if (d.layout != VK_NULL_HANDLE) {
        d.vk.vkDestroyPipelineLayout(d.device, d.layout, nullptr);
    }
    if (d.pool != VK_NULL_HANDLE) {
        d.vk.vkDestroyDescriptorPool(d.device, d.pool, nullptr);
    }
    if (d.set_layout != VK_NULL_HANDLE) {
        d.vk.vkDestroyDescriptorSetLayout(d.device, d.set_layout, nullptr);
    }
    if (d.sampler != VK_NULL_HANDLE) {
        d.vk.vkDestroySampler(d.device, d.sampler, nullptr);
    }
    d = Drawer();
}

void ui_vk_prepare(VkCommandBuffer cmd, uint32_t slot_index, const ui::Frame &frame) {
    if (!d.up || slot_index >= d.frames) {
        return;
    }
    Slot &slot = d.slots[slot_index];
    for (Buffer &b : slot.staging) { // (this slot's last frame has finished)
        free_buffer(b);
    }
    slot.staging.clear();
    d.prepared++;
    // the textures let go of before, that no frame in flight draws now
    for (size_t i = 0; i < d.releases.size();) {
        if (d.releases[i].when > d.prepared) {
            i++;
            continue;
        }
        auto found = d.textures.find(d.releases[i].id);
        if (found != d.textures.end()) {
            free_texture(found->second);
            d.textures.erase(found);
        }
        d.releases[i] = d.releases.back();
        d.releases.pop_back();
    }

    if (!d.white_uploaded) {
        const uint8_t white[4] = {255, 255, 255, 255};
        d.white_uploaded = upload(cmd, slot, d.white, 1, 1, white);
    }
    for (const ui::TextureUpload &u : frame.uploads) {
        Texture t;
        if (u.width <= 0 || u.height <= 0 || u.rgba.size() != size_t(u.width) * size_t(u.height) * 4 ||
            !make_texture(uint32_t(u.width), uint32_t(u.height), t)) {
            std::fprintf(stderr, "[ui] texture %u (%dx%d) could not be made\n", u.id, u.width, u.height);
            continue;
        }
        if (!upload(cmd, slot, t, uint32_t(u.width), uint32_t(u.height), u.rgba.data())) {
            free_texture(t);
            continue;
        }
        d.textures[u.id] = t;
    }
    for (ui::TextureId id : frame.releases) {
        d.releases.push_back({id, d.prepared + d.frames});
    }

    VkDeviceSize vertex_bytes = frame.vertices.size() * sizeof(ui::Vertex);
    VkDeviceSize index_bytes = frame.indices.size() * sizeof(uint32_t);
    if (vertex_bytes == 0 || index_bytes == 0 ||
        !fit_buffer(slot.vertices, vertex_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
        !fit_buffer(slot.indices, index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) {
        return;
    }
    std::memcpy(slot.vertices.map, frame.vertices.data(), size_t(vertex_bytes));
    std::memcpy(slot.indices.map, frame.indices.data(), size_t(index_bytes));
}

void ui_vk_draw(VkCommandBuffer cmd, uint32_t slot_index, const ui::Frame &frame, uint32_t width, uint32_t height) {
    if (!d.up || slot_index >= d.frames || width == 0 || height == 0 || frame.draws.empty()) {
        return;
    }
    Slot &slot = d.slots[slot_index];
    if (slot.vertices.size < frame.vertices.size() * sizeof(ui::Vertex) ||
        slot.indices.size < frame.indices.size() * sizeof(uint32_t) || !d.white_uploaded) {
        return; // (prepare could not make its buffers)
    }
    d.vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipeline);
    VkDeviceSize offset = 0;
    d.vk.vkCmdBindVertexBuffers(cmd, 0, 1, &slot.vertices.buffer, &offset);
    d.vk.vkCmdBindIndexBuffer(cmd, slot.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    VkViewport viewport = {0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    d.vk.vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkDescriptorSet bound = VK_NULL_HANDLE;
    for (const ui::Draw &draw : frame.draws) {
        if (uint64_t(draw.first_index) + draw.index_count > frame.indices.size()) {
            continue;
        }
        VkDescriptorSet set = d.white.set;
        if (draw.texture != 0) {
            auto found = d.textures.find(draw.texture);
            if (found == d.textures.end()) {
                continue; // (a texture that could not be made)
            }
            set = found->second.set;
        }
        if (set != bound) {
            d.vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout, 0, 1, &set, 0, nullptr);
            bound = set;
        }
        VkRect2D scissor = {{0, 0}, {width, height}};
        if (draw.clipped) {
            int32_t x0 = draw.clip.x < 0 ? 0 : draw.clip.x;
            int32_t y0 = draw.clip.y < 0 ? 0 : draw.clip.y;
            int64_t x1 = int64_t(draw.clip.x) + draw.clip.width;
            int64_t y1 = int64_t(draw.clip.y) + draw.clip.height;
            x1 = x1 > int64_t(width) ? int64_t(width) : x1;
            y1 = y1 > int64_t(height) ? int64_t(height) : y1;
            if (x1 <= x0 || y1 <= y0) {
                continue;
            }
            scissor = {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
        }
        d.vk.vkCmdSetScissor(cmd, 0, 1, &scissor);
        Push push = {{2.0f / float(width), 2.0f / float(height)}, {draw.translate_x, draw.translate_y}};
        d.vk.vkCmdPushConstants(cmd, d.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        d.vk.vkCmdDrawIndexed(cmd, draw.index_count, 1, draw.first_index, int32_t(draw.vertex_offset), 0);
    }
}
