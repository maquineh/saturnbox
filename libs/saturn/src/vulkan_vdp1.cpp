#include "saturn/saturn/vdp1_compute.hpp"

#if defined(SATURN_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "vdp1_compute_spv.hpp"
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <algorithm>
#include <bit>
#include <cstring>
#include <chrono>
#include <stdexcept>
#include <vector>

namespace saturn::platform {
namespace {

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed (VkResult " +
                                 std::to_string(result) + ")");
    }
}

#define INSTANCE_FUNCTIONS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define DEVICE_FUNCTIONS(X) \
    X(vkDestroyDevice) X(vkDeviceWaitIdle) X(vkGetDeviceQueue) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCmdCopyBuffer) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateComputePipelines) X(vkDestroyPipeline) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) \
    X(vkCmdDispatch) X(vkCmdPipelineBarrier) \
    X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkWaitForFences) X(vkQueueSubmit)

class VulkanVdp1 final : public Vdp1ComputeRenderer {
public:
    ~VulkanVdp1() override {
        if (device_) {
            if (vkDeviceWaitIdle) static_cast<void>(vkDeviceWaitIdle(device_));
            if (fence_) vkDestroyFence(device_, fence_, nullptr);
            if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
            if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
            if (shader_) vkDestroyShaderModule(device_, shader_, nullptr);
            if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
            if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
            if (descriptorLayout_) vkDestroyDescriptorSetLayout(device_, descriptorLayout_, nullptr);
            for (auto& buffer : buffers_) {
                if (buffer.mapped) vkUnmapMemory(device_, buffer.memory);
                if (buffer.buffer) vkDestroyBuffer(device_, buffer.buffer, nullptr);
                if (buffer.memory) vkFreeMemory(device_, buffer.memory, nullptr);
            }
            for (auto& buffer : stagingBuffers_) {
                if (buffer.mapped) vkUnmapMemory(device_, buffer.memory);
                if (buffer.buffer) vkDestroyBuffer(device_, buffer.buffer, nullptr);
                if (buffer.memory) vkFreeMemory(device_, buffer.memory, nullptr);
            }
            if (vkDestroyDevice) vkDestroyDevice(device_, nullptr);
        }
        if (instance_ && vkDestroyInstance) vkDestroyInstance(instance_, nullptr);
#if defined(_WIN32)
        if (library_) FreeLibrary(library_);
#else
        if (library_) dlclose(library_);
#endif
    }

    void initialize() {
#if defined(_WIN32)
        library_ = LoadLibraryW(L"vulkan-1.dll");
        const auto getProc = library_ ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(library_, "vkGetInstanceProcAddr")) : nullptr;
#else
        library_ = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        const auto getProc = library_ ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            dlsym(library_, "vkGetInstanceProcAddr")) : nullptr;
#endif
        if (!getProc) throw std::runtime_error("Vulkan loader unavailable");
        const auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(
            getProc(VK_NULL_HANDLE, "vkCreateInstance"));
        if (!createInstance) throw std::runtime_error("vkCreateInstance unavailable");
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "SaturnBox VDP1";
        application.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &application;
        check(createInstance(&instanceInfo, nullptr, &instance_), "vkCreateInstance");
#define LOAD_INSTANCE(name) \
        name = reinterpret_cast<PFN_##name>(getProc(instance_, #name)); \
        if (!name) throw std::runtime_error(#name " unavailable");
        INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE

        std::uint32_t count{};
        check(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), "vkEnumeratePhysicalDevices");
        for (auto candidate : devices) {
            VkPhysicalDeviceFeatures features{};
            vkGetPhysicalDeviceFeatures(candidate, &features);
            if (!features.shaderFloat64) continue;
            std::uint32_t familyCount{};
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
            for (std::uint32_t family = 0; family < familyCount; ++family) {
                if (families[family].queueCount != 0U &&
                    (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0U) {
                    physical_ = candidate;
                    queueFamily_ = family;
                    break;
                }
            }
            if (physical_) break;
        }
        if (!physical_) throw std::runtime_error("Vulkan compute with shaderFloat64 unavailable");
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical_, &properties);
        name_ = properties.deviceName;
        vkGetPhysicalDeviceMemoryProperties(physical_, &memoryProperties_);
        const float priority = 1.0F;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = queueFamily_;
        queueInfo.queueCount = 1U;
        queueInfo.pQueuePriorities = &priority;
        VkPhysicalDeviceFeatures enabled{};
        enabled.shaderFloat64 = VK_TRUE;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1U;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.pEnabledFeatures = &enabled;
        check(vkCreateDevice(physical_, &deviceInfo, nullptr, &device_), "vkCreateDevice");
#define LOAD_DEVICE(name) \
        name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device_, #name)); \
        if (!name) throw std::runtime_error(#name " unavailable");
        DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
        vkGetDeviceQueue(device_, queueFamily_, 0U, &queue_);
        constexpr VkDeviceSize vramBytes = 0x80000U;
        constexpr VkDeviceSize framebufferBytes = 512U * 256U * sizeof(std::uint32_t);
        constexpr VkDeviceSize commandBytes = 2000U * sizeof(Vdp1Primitive);
        createBuffer(buffers_[0], vramBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        createBuffer(buffers_[1], framebufferBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        createBuffer(buffers_[2], commandBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        createBuffer(stagingBuffers_[0], vramBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        createBuffer(stagingBuffers_[1], framebufferBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        createBuffer(stagingBuffers_[2], commandBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        for (std::uint32_t i = 0; i < bindings.size(); ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1U;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorLayout_),
              "vkCreateDescriptorSetLayout");
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3U};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1U;
        poolInfo.poolSizeCount = 1U;
        poolInfo.pPoolSizes = &poolSize;
        check(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_), "vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = descriptorPool_;
        allocation.descriptorSetCount = 1U;
        allocation.pSetLayouts = &descriptorLayout_;
        check(vkAllocateDescriptorSets(device_, &allocation, &descriptorSet_), "vkAllocateDescriptorSets");
        std::array<VkDescriptorBufferInfo, 3> bufferInfos{};
        std::array<VkWriteDescriptorSet, 3> writes{};
        for (std::uint32_t i = 0; i < writes.size(); ++i) {
            bufferInfos[i] = {buffers_[i].buffer, 0U, buffers_[i].size};
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = descriptorSet_;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1U;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &bufferInfos[i];
        }
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0U, nullptr);
        VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shaderInfo.codeSize = sizeof(kVdp1ComputeSpirv);
        shaderInfo.pCode = kVdp1ComputeSpirv;
        check(vkCreateShaderModule(device_, &shaderInfo, nullptr, &shader_), "vkCreateShaderModule");
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(BatchPush)};
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1U;
        pipelineLayoutInfo.pSetLayouts = &descriptorLayout_;
        pipelineLayoutInfo.pushConstantRangeCount = 1U;
        pipelineLayoutInfo.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_),
              "vkCreatePipelineLayout");
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.layout = pipelineLayout_;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = shader_;
        pipelineInfo.stage.pName = "main";
        check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1U, &pipelineInfo, nullptr, &pipeline_),
              "vkCreateComputePipelines");
        VkCommandPoolCreateInfo commandPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        commandPoolInfo.queueFamilyIndex = queueFamily_;
        check(vkCreateCommandPool(device_, &commandPoolInfo, nullptr, &commandPool_), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo commandAllocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandAllocation.commandPool = commandPool_;
        commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandAllocation.commandBufferCount = 1U;
        check(vkAllocateCommandBuffers(device_, &commandAllocation, &command_), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(device_, &fenceInfo, nullptr, &fence_), "vkCreateFence");
    }

    std::string_view deviceName() const noexcept override { return name_; }
    std::uint64_t dispatchCount() const noexcept override { return dispatches_; }
    std::uint64_t primitiveCount() const noexcept override { return primitives_; }
    double waitMilliseconds() const noexcept override { return waitMs_; }
    double copyMilliseconds() const noexcept override { return copyMs_; }

    void submit(std::span<const std::uint8_t> vram, std::span<const std::uint8_t> framebuffer,
                std::span<const Vdp1Primitive> primitives) override {
        if (primitives.empty()) return;
        if (pending_) throw std::runtime_error("VDP1 GPU storage reused before its fence completed");
        if (vram.size() != stagingBuffers_[0].size || framebuffer.size() != 0x40000U ||
            primitives.size_bytes() > stagingBuffers_[2].size) {
            throw std::runtime_error("VDP1 compute batch exceeds its storage buffers");
        }

        const auto bounds = batchBounds(primitives);
        if (!bounds.valid) return;
        const auto copyStart = Clock::now();
        std::memcpy(stagingBuffers_[0].mapped, vram.data(), vram.size());
        std::memcpy(stagingBuffers_[2].mapped, primitives.data(), primitives.size_bytes());
        auto* pixels = static_cast<std::uint32_t*>(stagingBuffers_[1].mapped);
        for (std::size_t i = 0; i < framebuffer.size() / 2U; ++i) {
            pixels[i] = (static_cast<std::uint32_t>(framebuffer[i*2U]) << 8U) | framebuffer[i*2U+1U];
        }
        copyMs_ += milliseconds(Clock::now() - copyStart);
        check(vkResetCommandBuffer(command_, 0U), "vkResetCommandBuffer");
        check(vkResetFences(device_, 1U, &fence_), "vkResetFences");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command_, &begin), "vkBeginCommandBuffer");
        for (std::size_t i = 0; i < buffers_.size(); ++i) {
            const VkBufferCopy copy{0U, 0U, i == 2U
                ? static_cast<VkDeviceSize>(primitives.size_bytes()) : buffers_[i].size};
            vkCmdCopyBuffer(command_, stagingBuffers_[i].buffer, buffers_[i].buffer, 1U, &copy);
        }
        VkMemoryBarrier uploadBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        uploadBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        uploadBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U,
                             1U, &uploadBarrier, 0U, nullptr, 0U, nullptr);
        vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_,
                                0U, 1U, &descriptorSet_, 0U, nullptr);
        const BatchPush push{static_cast<std::uint32_t>(primitives.size()),
            static_cast<std::uint32_t>(bounds.left), static_cast<std::uint32_t>(bounds.top),
            static_cast<std::uint32_t>(bounds.right), static_cast<std::uint32_t>(bounds.bottom)};
        vkCmdPushConstants(command_, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                           sizeof(push), &push);
        const auto width = static_cast<std::uint32_t>(bounds.right - bounds.left + 1);
        const auto height = static_cast<std::uint32_t>(bounds.bottom - bounds.top + 1);
        vkCmdDispatch(command_, (width + 7U) / 8U, (height + 7U) / 8U, 1U);
        VkMemoryBarrier downloadBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        downloadBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        downloadBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0U,
                             1U, &downloadBarrier, 0U, nullptr, 0U, nullptr);
        const VkBufferCopy framebufferCopy{0U, 0U, buffers_[1].size};
        vkCmdCopyBuffer(command_, buffers_[1].buffer, stagingBuffers_[1].buffer,
                        1U, &framebufferCopy);
        VkMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0U,
                             1U, &hostBarrier, 0U, nullptr, 0U, nullptr);
        check(vkEndCommandBuffer(command_), "vkEndCommandBuffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1U;
        submit.pCommandBuffers = &command_;
        check(vkQueueSubmit(queue_, 1U, &submit, fence_), "vkQueueSubmit");
        pending_ = true;
        ++dispatches_;
        primitives_ += primitives.size();
    }

    void finish(std::span<std::uint8_t> framebuffer) override {
        if (!pending_) return;
        const auto waitStart = Clock::now();
        check(vkWaitForFences(device_, 1U, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        waitMs_ += milliseconds(Clock::now() - waitStart);
        const auto copyStart = Clock::now();
        const auto* pixels = static_cast<const std::uint32_t*>(stagingBuffers_[1].mapped);
        for (std::size_t i = 0; i < framebuffer.size() / 2U; ++i) {
            framebuffer[i*2U] = static_cast<std::uint8_t>(pixels[i] >> 8U);
            framebuffer[i*2U+1U] = static_cast<std::uint8_t>(pixels[i]);
        }
        copyMs_ += milliseconds(Clock::now() - copyStart);
        pending_ = false;
    }

private:
    using Clock = std::chrono::steady_clock;
    static double milliseconds(Clock::duration duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    }
    struct Buffer {
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        VkDeviceSize size{};
        void* mapped{};
    };
    struct BatchPush {
        std::uint32_t count{}, left{}, top{}, right{}, bottom{};
    };
    static_assert(sizeof(BatchPush) == 20U);
    struct Bounds { int left{320}, top{256}, right{-1}, bottom{-1}; bool valid{}; };
    static Bounds batchBounds(std::span<const Vdp1Primitive> primitives) {
        Bounds bounds;
        for (const auto& primitive : primitives) {
            int left{}, top{}, right{}, bottom{};
            switch (primitive.flags[0]) {
            case Vdp1Primitive::Sprite:
                left = primitive.points[0];
                top = primitive.points[1];
                right = left + primitive.points[2] - 1;
                bottom = top + primitive.points[3] - 1;
                break;
            case Vdp1Primitive::Quad:
                left = std::min({primitive.points[0], primitive.points[2],
                                 primitive.points[4], primitive.points[6]});
                top = std::min({primitive.points[1], primitive.points[3],
                                primitive.points[5], primitive.points[7]});
                right = std::max({primitive.points[0], primitive.points[2],
                                  primitive.points[4], primitive.points[6]});
                bottom = std::max({primitive.points[1], primitive.points[3],
                                   primitive.points[5], primitive.points[7]});
                break;
            case Vdp1Primitive::Fill:
                left = primitive.points[0]; top = primitive.points[1];
                right = primitive.points[2]; bottom = primitive.points[3];
                break;
            default: continue;
            }
            left = std::max({left, primitive.clip[0], 0});
            top = std::max({top, primitive.clip[1], 0});
            right = std::min({right, primitive.clip[2], 319});
            bottom = std::min({bottom, primitive.clip[3], 255});
            if (left > right || top > bottom) continue;
            bounds.left = std::min(bounds.left, left);
            bounds.top = std::min(bounds.top, top);
            bounds.right = std::max(bounds.right, right);
            bounds.bottom = std::max(bounds.bottom, bottom);
            bounds.valid = true;
        }
        return bounds;
    }
    void createBuffer(Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags required,
                      VkMemoryPropertyFlags preferred = 0U) {
        buffer.size = size;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device_, &info, nullptr, &buffer.buffer), "vkCreateBuffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, buffer.buffer, &requirements);
        std::uint32_t type = UINT32_MAX;
        int bestScore = -1;
        for (std::uint32_t i = 0; i < memoryProperties_.memoryTypeCount; ++i) {
            const auto properties = memoryProperties_.memoryTypes[i].propertyFlags;
            if ((requirements.memoryTypeBits & (1U << i)) == 0U ||
                (properties & required) != required) continue;
            const auto score = std::popcount(static_cast<unsigned>(properties & preferred));
            if (score > bestScore) { type = i; bestScore = score; }
        }
        if (type == UINT32_MAX) throw std::runtime_error("Compatible Vulkan buffer memory unavailable");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        check(vkAllocateMemory(device_, &allocation, nullptr, &buffer.memory), "vkAllocateMemory");
        check(vkBindBufferMemory(device_, buffer.buffer, buffer.memory, 0U), "vkBindBufferMemory");
        if ((required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U) {
            check(vkMapMemory(device_, buffer.memory, 0U, size, 0U, &buffer.mapped), "vkMapMemory");
        }
    }
#define DECLARE(name) PFN_##name name{};
    INSTANCE_FUNCTIONS(DECLARE)
    DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE
#if defined(_WIN32)
    HMODULE library_{};
#else
    void* library_{};
#endif
    VkInstance instance_{};
    VkPhysicalDevice physical_{};
    VkPhysicalDeviceMemoryProperties memoryProperties_{};
    VkDevice device_{};
    VkQueue queue_{};
    std::uint32_t queueFamily_{};
    std::array<Buffer, 3> buffers_{};
    std::array<Buffer, 3> stagingBuffers_{};
    VkDescriptorSetLayout descriptorLayout_{};
    VkDescriptorPool descriptorPool_{};
    VkDescriptorSet descriptorSet_{};
    VkShaderModule shader_{};
    VkPipelineLayout pipelineLayout_{};
    VkPipeline pipeline_{};
    VkCommandPool commandPool_{};
    VkCommandBuffer command_{};
    VkFence fence_{};
    std::string name_;
    std::uint64_t dispatches_{}, primitives_{};
    bool pending_{};
    double waitMs_{}, copyMs_{};
};
#undef INSTANCE_FUNCTIONS
#undef DEVICE_FUNCTIONS
} // namespace

std::shared_ptr<Vdp1ComputeRenderer> createVulkanVdp1(std::string& error) {
    try {
        auto renderer = std::make_shared<VulkanVdp1>();
        renderer->initialize();
        error.clear();
        return renderer;
    } catch (const std::exception& exception) {
        error = exception.what();
        return {};
    }
}
} // namespace saturn::platform
#else
namespace saturn::platform {
std::shared_ptr<Vdp1ComputeRenderer> createVulkanVdp1(std::string& error) {
    error = "Vulkan backend disabled at build time (SATURNBOX_VULKAN=OFF)";
    return {};
}
} // namespace saturn::platform
#endif
