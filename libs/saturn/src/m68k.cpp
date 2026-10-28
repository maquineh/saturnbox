#include "saturn/saturn/m68k.hpp"

#include <m68k.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <utility>

namespace saturn::platform {
namespace {

constexpr std::uint32_t kResetCyclePenalty = 40U;
std::atomic<M68k*> activeCpu{};

[[nodiscard]] M68k& active() noexcept {
    return *activeCpu.load(std::memory_order_acquire);
}

} // namespace

struct M68kBusBridge {
    static std::uint8_t read8(std::uint32_t address) {
        const auto& callback = active().bus_.read8;
        return callback ? callback(address) : 0xFFU;
    }
    static std::uint16_t read16(std::uint32_t address) {
        const auto& callback = active().bus_.read16;
        return callback ? callback(address) : 0xFFFFU;
    }
    static std::uint32_t read32(std::uint32_t address) {
        const auto& callback = active().bus_.read32;
        return callback ? callback(address) : 0xFFFFFFFFU;
    }
    static void write8(std::uint32_t address, std::uint8_t value) {
        const auto& callback = active().bus_.write8;
        if (callback) callback(address, value);
    }
    static void write16(std::uint32_t address, std::uint16_t value) {
        const auto& callback = active().bus_.write16;
        if (callback) callback(address, value);
    }
    static void write32(std::uint32_t address, std::uint32_t value) {
        const auto& callback = active().bus_.write32;
        if (callback) callback(address, value);
    }
};

M68k::M68k(Bus bus) : bus_(std::move(bus)) {
    M68k* expected = nullptr;
    if (!activeCpu.compare_exchange_strong(expected, this,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        throw std::logic_error("Musashi uses process-global CPU state; only one M68k may exist");
    }
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
}

M68k::~M68k() {
    M68k* expected = this;
    (void)activeCpu.compare_exchange_strong(expected, nullptr,
        std::memory_order_acq_rel, std::memory_order_acquire);
}

void M68k::reset() {
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    // Musashi returns its 40-cycle reset delay from execute(0) without
    // fetching or executing an instruction. Keep that delay in the wrapper's
    // budget accounting so small execute() budgets remain bounded.
    (void)m68k_execute(0);
    irqLevel_ = 0U;
    stats_ = {};
    resetCyclesPending_ = kResetCyclePenalty;
    reset_ = true;
}

std::uint32_t M68k::execute(std::uint32_t cycleBudget) {
    if (!reset_) {
        throw std::logic_error("M68k::reset must be called before execute");
    }
    if (cycleBudget == 0U) return 0U;

    auto remaining = cycleBudget;
    const auto resetCycles = std::min(resetCyclesPending_, remaining);
    resetCyclesPending_ -= resetCycles;
    remaining -= resetCycles;
    auto totalCycles = resetCycles;
    if (remaining != 0U) {
        const auto budget = static_cast<int>(std::min<std::uint32_t>(
            remaining, static_cast<std::uint32_t>(std::numeric_limits<int>::max())));
        const auto cycles = std::max(m68k_execute(budget), 0);
        totalCycles += static_cast<std::uint32_t>(cycles);
    }
    ++stats_.executeCalls;
    stats_.lastCycleCount = totalCycles;
    stats_.cyclesExecuted += stats_.lastCycleCount;
    return stats_.lastCycleCount;
}

void M68k::setIrq(std::uint8_t level) {
    if (level > 7U) throw std::invalid_argument("68k IRQ level must be in [0, 7]");
    irqLevel_ = level;
    m68k_set_irq(irqLevel_);
}

std::uint32_t M68k::pc() const noexcept {
    return m68k_get_reg(nullptr, M68K_REG_PC);
}

const M68k::Stats& M68k::stats() const noexcept { return stats_; }

} // namespace saturn::platform

extern "C" unsigned int m68k_read_memory_8(unsigned int address) {
    return saturn::platform::M68kBusBridge::read8(address);
}

extern "C" unsigned int m68k_read_memory_16(unsigned int address) {
    return saturn::platform::M68kBusBridge::read16(address);
}

extern "C" unsigned int m68k_read_memory_32(unsigned int address) {
    return saturn::platform::M68kBusBridge::read32(address);
}

extern "C" void m68k_write_memory_8(unsigned int address, unsigned int value) {
    saturn::platform::M68kBusBridge::write8(address, static_cast<std::uint8_t>(value));
}

extern "C" void m68k_write_memory_16(unsigned int address, unsigned int value) {
    saturn::platform::M68kBusBridge::write16(address, static_cast<std::uint16_t>(value));
}

extern "C" void m68k_write_memory_32(unsigned int address, unsigned int value) {
    saturn::platform::M68kBusBridge::write32(address, value);
}
