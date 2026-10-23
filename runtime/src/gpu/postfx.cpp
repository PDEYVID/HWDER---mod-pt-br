// Post-processing: the game image is blitted into an RGBA8 texture and one compute pass writes the
// result - FXAA (Lottes' original console variant: 5 taps for edge direction, 4 along it, no search
// loop) followed by a sharpening step clamped to the 3x3 neighbourhood's range (no halos). The SPIR-V
// is generated at startup with the translator's own SpvBuilder, so no shader compiler is needed.
#include "gpu/postfx.h"

#include <algorithm>
#include <vector>

#include "gpu/display.h"
#include "gpu/settings.h"
#include "shader/spv_builder.h"

namespace postfx {

namespace {

struct State {
    bool init_failed = false;
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    // per size
    u32 w = 0, h = 0;
    VkImage in = VK_NULL_HANDLE, out = VK_NULL_HANDLE;
    VkDeviceMemory in_mem = VK_NULL_HANDLE, out_mem = VK_NULL_HANDLE;
    VkImageView in_view = VK_NULL_HANDLE, out_view = VK_NULL_HANDLE;
};
State g;

VkDevice dev() { return display::device().dev; }

struct Push {
    float inv_size[2];
    float sharpen;  // 0..1
    float fxaa;     // 0 or 1
};

std::vector<u32> build_spirv() {
    using namespace shader;
    using namespace spv;
    SpvBuilder b;
    b.capability(CapabilityShader);
    SpvBuilder::put(b.memory_model, OpMemoryModel, {AddressingModelLogical, MemoryModelGLSL450});
    const Id tvoid = b.t_void(), tb = b.t_bool(), tf = b.t_float(), tu = b.t_uint(), ti = b.t_int();
    const Id tv2 = b.t_vec(tf, 2), tv3 = b.t_vec(tf, 3), tv4 = b.t_vec(tf, 4), tbv4 = b.t_vec(tb, 4);
    const Id tuv2 = b.t_vec(tu, 2), tuv3 = b.t_vec(tu, 3), tiv2 = b.t_vec(ti, 2);
    const Id timg = b.type(OpTypeImage, {tf, Dim2D, 0, 0, 0, 1, ImageFormatUnknown});
    const Id tsimg = b.type(OpTypeSampledImage, {timg});
    const Id tout = b.type(OpTypeImage, {tf, Dim2D, 0, 0, 0, 2, ImageFormatRgba8});
    const Id vsrc = b.global_var(b.t_ptr(StorageClassUniformConstant, tsimg), StorageClassUniformConstant);
    b.decorate(vsrc, DecorationDescriptorSet, {0});
    b.decorate(vsrc, DecorationBinding, {0});
    const Id vout = b.global_var(b.t_ptr(StorageClassUniformConstant, tout), StorageClassUniformConstant);
    b.decorate(vout, DecorationDescriptorSet, {0});
    b.decorate(vout, DecorationBinding, {1});
    b.decorate(vout, DecorationNonReadable);
    const Id tpc = b.t_struct_fresh({tv2, tf, tf});
    b.decorate(tpc, DecorationBlock);
    b.member_decorate(tpc, 0, DecorationOffset, {0});
    b.member_decorate(tpc, 1, DecorationOffset, {8});
    b.member_decorate(tpc, 2, DecorationOffset, {12});
    const Id vpc = b.global_var(b.t_ptr(StorageClassPushConstant, tpc), StorageClassPushConstant);
    const Id vgid = b.global_var(b.t_ptr(StorageClassInput, tuv3), StorageClassInput);
    b.decorate(vgid, DecorationBuiltIn, {BuiltInGlobalInvocationId});

    const Id fn = b.begin_function(tvoid, b.t_func(tvoid));
    {
        std::vector<u32> ep{ExecutionModelGLCompute, fn};
        SpvBuilder::put_str(ep, "main");
        ep.push_back(vgid);  // SPIR-V 1.0: only Input/Output variables in the interface
        SpvBuilder::put(b.entry_points, OpEntryPoint, ep);
        SpvBuilder::put(b.exec_modes, OpExecutionMode, {fn, ExecutionModeLocalSize, 8, 8, 1});
    }
    auto f = [&](float v) { return b.c_float(v); };
    auto add = [&](Id t, Id x, Id y) { return b.op(OpFAdd, t, {x, y}); };
    auto sub = [&](Id t, Id x, Id y) { return b.op(OpFSub, t, {x, y}); };
    auto mul = [&](Id t, Id x, Id y) { return b.op(OpFMul, t, {x, y}); };
    auto scale = [&](Id t, Id v, Id s) { return b.op(OpVectorTimesScalar, t, {v, s}); };
    auto fmin = [&](Id t, Id x, Id y) { return b.ext(t, GLSLstd450FMin, {x, y}); };
    auto fmax = [&](Id t, Id x, Id y) { return b.ext(t, GLSLstd450FMax, {x, y}); };
    auto vec2 = [&](Id x, Id y) { return b.op(OpCompositeConstruct, tv2, {x, y}); };
    auto splat4 = [&](Id c) { return b.op(OpCompositeConstruct, tbv4, {c, c, c, c}); };

    const Id gid = b.load(tuv3, vgid);
    const Id gxy = b.op(OpVectorShuffle, tuv2, {gid, gid, 0, 1});
    const Id fx = b.op(OpConvertUToF, tf, {b.op(OpCompositeExtract, tu, {gid, 0})});
    const Id fy = b.op(OpConvertUToF, tf, {b.op(OpCompositeExtract, tu, {gid, 1})});
    const Id inv = b.load(tv2, b.op(OpAccessChain, b.t_ptr(StorageClassPushConstant, tv2), {vpc, b.c_int(0)}));
    const Id sharp = b.load(tf, b.op(OpAccessChain, b.t_ptr(StorageClassPushConstant, tf), {vpc, b.c_int(1)}));
    const Id use_fxaa = b.load(tf, b.op(OpAccessChain, b.t_ptr(StorageClassPushConstant, tf), {vpc, b.c_int(2)}));
    const Id uv = mul(tv2, vec2(add(tf, fx, f(0.5f)), add(tf, fy, f(0.5f))), inv);
    const Id simg = b.load(tsimg, vsrc);
    auto tex_at = [&](Id coord) {
        return b.op(OpImageSampleExplicitLod, tv4, {simg, coord, ImageOperandsLodMask, f(0.0f)});
    };
    auto tap = [&](float ox, float oy) { return tex_at(add(tv2, uv, mul(tv2, b.c_composite(tv2, {f(ox), f(oy)}), inv))); };
    const Id luma_w = b.c_composite(tv3, {f(0.299f), f(0.587f), f(0.114f)});
    auto luma = [&](Id c) { return b.op(OpDot, tf, {b.op(OpVectorShuffle, tv3, {c, c, 0, 1, 2}), luma_w}); };

    // ---- FXAA
    const Id rgbM = tap(0, 0), rgbNW = tap(-1, -1), rgbNE = tap(1, -1), rgbSW = tap(-1, 1), rgbSE = tap(1, 1);
    const Id lM = luma(rgbM), lNW = luma(rgbNW), lNE = luma(rgbNE), lSW = luma(rgbSW), lSE = luma(rgbSE);
    const Id lmin = fmin(tf, lM, fmin(tf, fmin(tf, lNW, lNE), fmin(tf, lSW, lSE)));
    const Id lmax = fmax(tf, lM, fmax(tf, fmax(tf, lNW, lNE), fmax(tf, lSW, lSE)));
    const Id dir_x = b.op(OpFNegate, tf, {sub(tf, add(tf, lNW, lNE), add(tf, lSW, lSE))});
    const Id dir_y = sub(tf, add(tf, lNW, lSW), add(tf, lNE, lSE));
    const Id reduce = fmax(tf, mul(tf, add(tf, add(tf, lNW, lNE), add(tf, lSW, lSE)), f(0.25f / 8.0f)), f(1.0f / 128.0f));
    const Id rcp_min = b.op(OpFDiv, tf, {f(1.0f), add(tf, fmin(tf, b.ext(tf, GLSLstd450FAbs, {dir_x}), b.ext(tf, GLSLstd450FAbs, {dir_y})), reduce)});
    const Id dir = mul(tv2, b.ext(tv2, GLSLstd450FClamp, {scale(tv2, vec2(dir_x, dir_y), rcp_min), b.c_composite(tv2, {f(-8), f(-8)}),
                                                           b.c_composite(tv2, {f(8), f(8)})}),
                       inv);
    auto along = [&](float k) { return tex_at(add(tv2, uv, scale(tv2, dir, f(k)))); };
    const Id rgbA = scale(tv4, add(tv4, along(1.0f / 3.0f - 0.5f), along(2.0f / 3.0f - 0.5f)), f(0.5f));
    const Id rgbB = add(tv4, scale(tv4, rgbA, f(0.5f)), scale(tv4, add(tv4, along(-0.5f), along(0.5f)), f(0.25f)));
    const Id lB = luma(rgbB);
    const Id outside = b.op(OpLogicalOr, tb, {b.op(OpFOrdLessThan, tb, {lB, lmin}), b.op(OpFOrdGreaterThan, tb, {lB, lmax})});
    const Id aa = b.op(OpSelect, tv4, {splat4(outside), rgbA, rgbB});
    const Id base = b.op(OpSelect, tv4, {splat4(b.op(OpFOrdGreaterThan, tb, {use_fxaa, f(0.5f)})), aa, rgbM});

    // ---- sharpening: unsharp mask from the cross neighbours, clamped to the 3x3 range
    const Id n = tap(0, -1), s = tap(0, 1), w = tap(-1, 0), e = tap(1, 0);
    const Id ring = add(tv4, add(tv4, n, s), add(tv4, w, e));
    const Id detail = sub(tv4, scale(tv4, base, f(4.0f)), ring);
    const Id sharpened = add(tv4, base, scale(tv4, detail, mul(tf, sharp, f(0.25f))));
    Id lo = fmin(tv4, fmin(tv4, n, s), fmin(tv4, w, e)), hi = fmax(tv4, fmax(tv4, n, s), fmax(tv4, w, e));
    for (Id c : {rgbNW, rgbNE, rgbSW, rgbSE, base}) lo = fmin(tv4, lo, c), hi = fmax(tv4, hi, c);
    const Id result = b.ext(tv4, GLSLstd450FClamp, {sharpened, lo, hi});

    b.op0(OpImageWrite, {b.load(tout, vout), b.bitcast(tiv2, gxy), result});
    b.terminate(OpReturn);
    b.end_function();
    return b.assemble(0x10000);
}

u32 mem_type(u32 bits, VkMemoryPropertyFlags want) {
    const auto& m = display::device().mem;
    for (u32 i = 0; i < m.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (m.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}

bool make_image(u32 w, u32 h, VkImageUsageFlags usage, VkImage& img, VkDeviceMemory& mem, VkImageView& view) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {w, h, 1};
    ici.mipLevels = ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    if (vkCreateImage(dev(), &ici, nullptr, &img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev(), img, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == ~0u || vkAllocateMemory(dev(), &mai, nullptr, &mem) != VK_SUCCESS) return false;
    vkBindImageMemory(dev(), img, mem, 0);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkCreateImageView(dev(), &vci, nullptr, &view) == VK_SUCCESS;
}

bool init_pipeline() {
    if (g.pipe) return true;
    if (g.init_failed) return false;
    g.init_failed = true;  // cleared on success
    std::vector<u32> spirv = build_spirv();
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = spirv.size() * 4;
    smi.pCode = spirv.data();
    if (vkCreateShaderModule(dev(), &smi, nullptr, &g.module) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding binds[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 2;
    dli.pBindings = binds;
    if (vkCreateDescriptorSetLayout(dev(), &dli, nullptr, &g.set_layout) != VK_SUCCESS) return false;
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &g.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(dev(), &pli, nullptr, &g.layout) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = g.module;
    cpi.stage.pName = "main";
    cpi.layout = g.layout;
    if (vkCreateComputePipelines(dev(), VK_NULL_HANDLE, 1, &cpi, nullptr, &g.pipe) != VK_SUCCESS) return false;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev(), &sci, nullptr, &g.sampler) != VK_SUCCESS) return false;
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev(), &dpi, nullptr, &g.pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = g.pool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &g.set_layout;
    if (vkAllocateDescriptorSets(dev(), &dai, &g.set) != VK_SUCCESS) return false;
    g.init_failed = false;
    hw_log("postfx: pipeline ready (%zu SPIR-V words)", spirv.size());
    return true;
}

void destroy_images() {
    for (VkImageView* v : {&g.in_view, &g.out_view})
        if (*v) vkDestroyImageView(dev(), *v, nullptr), *v = VK_NULL_HANDLE;
    for (VkImage* i : {&g.in, &g.out})
        if (*i) vkDestroyImage(dev(), *i, nullptr), *i = VK_NULL_HANDLE;
    for (VkDeviceMemory* m : {&g.in_mem, &g.out_mem})
        if (*m) vkFreeMemory(dev(), *m, nullptr), *m = VK_NULL_HANDLE;
    g.w = g.h = 0;
}

void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags src_stage,
             VkAccessFlags src_access, VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace

bool enabled() {
    const settings::Video& v = settings::video();
    return (v.fxaa || v.sharpen > 0) && !g.init_failed;
}

bool needs_rebuild(u32 w, u32 h) { return !g.pipe || g.w != w || g.h != h; }

void rebuild(u32 w, u32 h) {
    if (!init_pipeline()) {
        hw_log("postfx: unavailable on this GPU - post-processing disabled");
        return;
    }
    destroy_images();
    // The output is rounded up to the 8x8 workgroup grid: every invocation writes inside the image.
    u32 ow = (w + 7) & ~7u, oh = (h + 7) & ~7u;
    if (!make_image(w, h, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, g.in, g.in_mem, g.in_view) ||
        !make_image(ow, oh, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, g.out, g.out_mem, g.out_view)) {
        hw_log("postfx: cannot allocate %ux%u images", w, h);
        destroy_images();
        return;
    }
    VkDescriptorImageInfo src_info{g.sampler, g.in_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo dst_info{VK_NULL_HANDLE, g.out_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    wr[0].dstSet = wr[1].dstSet = g.set;
    wr[0].dstBinding = 0;
    wr[0].descriptorCount = 1;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr[0].pImageInfo = &src_info;
    wr[1].dstBinding = 1;
    wr[1].descriptorCount = 1;
    wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    wr[1].pImageInfo = &dst_info;
    vkUpdateDescriptorSets(dev(), 2, wr, 0, nullptr);
    g.w = w;
    g.h = h;
    hw_log("postfx: %ux%u", w, h);
}

VkImage record(VkCommandBuffer cb, VkImage src, u32 w, u32 h) {
    if (!g.pipe || g.w != w || g.h != h) return VK_NULL_HANDLE;
    const settings::Video& v = settings::video();
    // in <- game image (any format; the blit converts to RGBA8)
    barrier(cb, g.in, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = blit.dstOffsets[1] = {(s32)w, (s32)h, 1};
    vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, g.in, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    barrier(cb, g.in, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    barrier(cb, g.out, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 0, 1, &g.set, 0, nullptr);
    Push pc{{1.0f / w, 1.0f / h}, std::clamp(v.sharpen, 0, 100) / 100.0f, v.fxaa ? 1.0f : 0.0f};
    vkCmdPushConstants(cb, g.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    barrier(cb, g.out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    return g.out;
}

}  // namespace postfx
