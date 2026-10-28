#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace saturn::platform {

// Internal SCSP (YMF292-F) effect DSP: 128 program steps at 24-bit precision
// with 64 coefficients, 32 memory-address words, a 128-entry temp delay line
// and 16 effect registers. The delay memory itself lives in sound RAM, so this
// device needs the same RAM image the slots read.
//
// Timing, packing and the program encoding follow MAME's scspdsp.cpp
// (BSD-3-Clause, ElSemi / R. Belmont); see third_party/MAME-SCSP-LICENSE.txt.
// The Saturn connects this unit to the slot sends (ISEL/IMXL) and to the
// effect returns (EFSDL/EFPAN of slots 0-15).
class ScspDsp final {
public:
    static constexpr std::size_t kCoefCount = 64;
    static constexpr std::size_t kAddressCount = 32;
    static constexpr std::size_t kProgramWords = 512; // 128 steps * 4 words.
    static constexpr std::size_t kStepCount = 128;
    static constexpr std::size_t kTempCount = 128;
    static constexpr std::size_t kMemCount = 32;
    static constexpr std::size_t kMixCount = 16;
    static constexpr std::size_t kEffectCount = 16;
    static constexpr std::size_t kExternalCount = 2;
    // 512 KiB of sound RAM seen as 16-bit big-endian words.
    static constexpr std::size_t kRamWords = 0x40000;

    struct Stats {
        std::uint64_t steps{};         // Executed program steps.
        std::uint64_t samples{};       // Samples that ran the program.
        std::uint64_t reads{};         // Delay-memory word reads.
        std::uint64_t writes{};        // Delay-memory word writes.
        std::uint64_t droppedWrites{}; // Writes with no writable RAM image.
    };

    ScspDsp();

    void reset();
    // Re-scans the program for its last non-zero step and leaves the stopped
    // state. Writing MPRO word 0x3F0 (register offset 0xBF0) calls this.
    void start();

    [[nodiscard]] bool stopped() const noexcept { return stopped_; }
    [[nodiscard]] std::uint8_t lastStep() const noexcept { return lastStep_; }
    [[nodiscard]] std::uint16_t ringBase() const noexcept { return ringBase_; }
    [[nodiscard]] std::uint32_t ringLength() const noexcept { return ringLength_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    // Ring-buffer description from the MEM4B word (RBP/RBL). Length is in
    // 16-bit words and always a power of two.
    void setRingBuffer(std::uint16_t base, std::uint16_t length) noexcept;

    // Adds one slot send sample to MIXS[index]. `sample` uses the same 16-bit
    // scale as the slot output multiplied by 16, matching MAME's
    // (sample * table) >> (SHIFT - 2) feed.
    void mixSample(std::size_t index, std::int32_t sample) noexcept;
    [[nodiscard]] std::int32_t mixRegister(std::size_t index) const noexcept;

    // Register window offsets 0x700-0xEE3. Values are 16-bit big-endian words
    // on the 68K/SH-2 bus; byte writes reach here already merged.
    void registerWord(std::uint16_t offset, std::uint16_t value) noexcept;
    [[nodiscard]] std::uint16_t readWord(std::uint16_t offset) const noexcept;
    // Byte lanes of the read-only DSP state at 0xE00-0xEE3, or nullopt.
    [[nodiscard]] std::optional<std::uint8_t> readByte(std::uint16_t offset) const noexcept;
    [[nodiscard]] static bool isReadableRegister(std::uint16_t offset) noexcept;

    // Runs the whole program once: one sample of effect processing.
    // `ram` is the current sound RAM (read only), `ramOut` may be null when the
    // caller cannot provide a writable image; delay-memory writes are then
    // counted as dropped. `ramBytes` bounds both views: short images in tests
    // must not turn a delay-line address into an out-of-bounds access.
    void step(const std::uint8_t* ram, std::uint8_t* ramOut, std::size_t ramBytes);

    [[nodiscard]] const std::array<std::int32_t, kEffectCount>& effectRegisters() const noexcept {
        return effectRegisters_;
    }

private:
    [[nodiscard]] std::int32_t ramWord(const std::uint8_t* ram, std::size_t word) const noexcept;
    bool writeRamWord(std::uint8_t* ramOut, std::size_t word, std::uint16_t value) noexcept;
    std::array<std::uint16_t, kCoefCount> coefficients_{};
    std::array<std::uint16_t, kAddressCount> addresses_{};
    std::array<std::uint16_t, kProgramWords> program_{};
    std::array<std::int32_t, kTempCount> temp_{};
    std::array<std::int32_t, kMemCount> memory_{};
    std::array<std::int32_t, kMixCount> mixRegisters_{};
    std::array<std::int32_t, kExternalCount> external_{};
    std::array<std::int32_t, kEffectCount> effectRegisters_{};

    std::uint16_t ringBase_{};
    std::uint32_t ringLength_{8U * 1024U};
    std::size_t ramWordLimit_{kRamWords};
    std::int32_t dec_{};
    bool stopped_{true};
    std::uint8_t lastStep_{};
    Stats stats_{};
};

} // namespace saturn::platform
