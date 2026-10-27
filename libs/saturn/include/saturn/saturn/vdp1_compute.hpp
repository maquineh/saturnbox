#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace saturn::platform {

// Six vec4 fields shared with the std430 compute shader. Framebuffer colors
// remain Saturn RGB555/palette codes; VDP2 still resolves their priorities.
struct alignas(16) Vdp1Primitive {
    enum Kind : std::uint32_t { Sprite, Quad, Fill };
    std::array<std::uint32_t, 4> flags{}; // kind, control, PMOD, color
    std::array<std::int32_t, 8> points{}; // quad ABCD or sprite x,y,width,height
    std::array<std::int32_t, 4> clip{};   // inclusive left,top,right,bottom
    std::array<std::uint32_t, 4> texture{}; // source,width,height,reserved
    std::array<std::uint32_t, 4> shades{};
};
static_assert(sizeof(Vdp1Primitive) == 96U);

class Vdp1ComputeRenderer {
public:
    virtual ~Vdp1ComputeRenderer() = default;
    [[nodiscard]] virtual std::string_view deviceName() const noexcept = 0;
    // One invocation owns one framebuffer pixel and applies commands in order.
    // Input snapshots stay owned by the device until finish; the host can run
    // guest CPU/VDP2 work between submission and framebuffer visibility.
    virtual void submit(std::span<const std::uint8_t> vram,
                        std::span<const std::uint8_t> framebuffer,
                        std::span<const Vdp1Primitive> primitives) = 0;
    virtual void finish(std::span<std::uint8_t> framebuffer) = 0;
    [[nodiscard]] virtual std::uint64_t dispatchCount() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t primitiveCount() const noexcept = 0;
    [[nodiscard]] virtual double waitMilliseconds() const noexcept = 0;
    [[nodiscard]] virtual double copyMilliseconds() const noexcept = 0;
};

// Loads the system Vulkan driver; no SDK DLL is needed at runtime. A failed
// explicit selection is reported to the caller, rather than silently replayed.
[[nodiscard]] std::shared_ptr<Vdp1ComputeRenderer> createVulkanVdp1(std::string& error);

} // namespace saturn::platform
