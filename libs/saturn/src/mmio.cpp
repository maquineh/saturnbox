#include "saturn/saturn/mmio.hpp"
#include "saturn/sh2/module_lifter.hpp"

namespace saturn::platform {
namespace {

constexpr core::Address kCacheBypassAliasDelta = 0x20000000U;
constexpr core::Address kSmpcOreg2 = 0x20100025U;
constexpr core::Address kScuBase = 0x05FE0000U;
constexpr core::Address kScuInterruptMask = kScuBase + 0xA0U;
constexpr core::Address kScuInterruptStatus = kScuBase + 0xA4U;
constexpr core::Address kScuAbusAcknowledge = kScuBase + 0xA8U;
constexpr core::Address kSh2DivisionBase = 0xFFFFFF00U;
constexpr core::Address kSh2DivisionDivisor = kSh2DivisionBase + 0x00U;
constexpr core::Address kSh2DivisionDividendLow = kSh2DivisionBase + 0x04U;
constexpr core::Address kSh2DivisionDividendHigh = kSh2DivisionBase + 0x08U;
constexpr core::Address kSh2DivisionControl = kSh2DivisionBase + 0x10U;
constexpr core::Address kSh2DivisionQuotient = kSh2DivisionBase + 0x14U;

// SH-2 free-running timer registers. FRC is byte-addressable in big-endian
// order at 0xFFFFFE12/13; TCR.CKS selects phi/8, /32, /128, or external clock.
constexpr core::Address kSh2FrtBase = 0xFFFFFE10U;
constexpr core::Address kSh2FrtEnd = 0xFFFFFE19U;
constexpr core::Address kSh2FrtStatus = kSh2FrtBase + 1U;
constexpr core::Address kSh2FrtCounterHigh = kSh2FrtBase + 2U;
constexpr core::Address kSh2FrtCounterLow = kSh2FrtBase + 3U;
constexpr core::Address kSh2FrtControl = kSh2FrtBase + 6U;
constexpr std::uint8_t kSh2FrtOverflowFlag = 0x02U;

// SH-2 built-in DMA controller (master CPU).  These registers live in the
// same on-chip window as the division unit but are not Saturn external MMIO.
constexpr core::Address kSh2DmaSar0 = 0xFFFFFF80U;
constexpr core::Address kSh2DmaDar0 = 0xFFFFFF84U;
constexpr core::Address kSh2DmaTcr0 = 0xFFFFFF88U;
constexpr core::Address kSh2DmaChcr0 = 0xFFFFFF8CU;
constexpr core::Address kSh2DmaSar1 = 0xFFFFFF90U;
constexpr core::Address kSh2DmaDar1 = 0xFFFFFF94U;
constexpr core::Address kSh2DmaTcr1 = 0xFFFFFF98U;
constexpr core::Address kSh2DmaChcr1 = 0xFFFFFF9CU;
constexpr core::Address kSh2DmaVcr0 = 0xFFFFFFA0U;
constexpr core::Address kSh2DmaVcr1 = 0xFFFFFFA8U;
constexpr core::Address kSh2DmaOr = 0xFFFFFFB0U;

[[nodiscard]] constexpr std::uint32_t widthMask(std::uint8_t width) noexcept {
    return width == 1U ? 0xFFU : width == 2U ? 0xFFFFU : 0xFFFFFFFFU;
}

[[nodiscard]] constexpr bool isSh2DivisionRegister(core::Address address) noexcept {
    return address >= kSh2DivisionBase && address <= kSh2DivisionQuotient;
}

[[nodiscard]] constexpr bool isSh2FrtRegister(core::Address address,
                                              std::uint8_t width) noexcept {
    return width != 0U && address >= kSh2FrtBase && address <= kSh2FrtEnd &&
           static_cast<std::uint64_t>(address) + width - 1U <= kSh2FrtEnd;
}

[[nodiscard]] constexpr bool isSh2DmaRegister(core::Address address) noexcept {
    switch (address) {
    case kSh2DmaSar0: case kSh2DmaDar0: case kSh2DmaTcr0: case kSh2DmaChcr0:
    case kSh2DmaSar1: case kSh2DmaDar1: case kSh2DmaTcr1: case kSh2DmaChcr1:
    case kSh2DmaVcr0: case kSh2DmaVcr1: case kSh2DmaOr:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] core::Address deviceAddress(AddressSpace space,
                                           core::Address address) noexcept {
    if ((space == AddressSpace::CdBlock || space == AddressSpace::Vdp1 ||
         space == AddressSpace::Vdp2 || space == AddressSpace::Scsp ||
         space == AddressSpace::Scu) &&
        address >= kCacheBypassAliasDelta && address < 0x30000000U) {
        return address - kCacheBypassAliasDelta;
    }
    return address;
}

} // namespace

void MmioInterceptor::setCpuTiming(std::uint32_t cyclesPerSecond,
                                    std::size_t blocksPerSecond) noexcept {
    if (cyclesPerSecond == 0U || blocksPerSecond == 0U) return;
    sh2CyclesPerSecond_ = cyclesPerSecond;
    sh2BlocksPerSecond_ = blocksPerSecond;
    sh2FrtBlockRemainder_ = 0U;
}

core::Address MmioInterceptor::backingAddress(core::Address address) const noexcept {
    return canonicalizeRamAddress(address);
}

void MmioInterceptor::setExecutionContext(const sir::ExecutionContext& context) {
    if (!sh2FrtStepInitialized_ || context.step < sh2FrtLastStep_) {
        sh2FrtStepInitialized_ = true;
        sh2FrtLastStep_ = context.step;
    } else {
        const auto elapsedBlocks = context.step - sh2FrtLastStep_;
        sh2FrtLastStep_ = context.step;
        const auto clockSelect =
            sh2FrtRegisters_[kSh2FrtControl - kSh2FrtBase] & 0x03U;
        const std::uint64_t prescaler = clockSelect == 0U ? 8U :
            clockSelect == 1U ? 32U : clockSelect == 2U ? 128U : 0U;
        if (prescaler != 0U) {
            const auto denominator = sh2BlocksPerSecond_ * prescaler;
            const auto accumulated = sh2FrtBlockRemainder_ +
                static_cast<std::uint64_t>(elapsedBlocks) * sh2CyclesPerSecond_;
            const auto ticks = accumulated / denominator;
            sh2FrtBlockRemainder_ = accumulated % denominator;
            const auto advanced = static_cast<std::uint64_t>(
                sh2FreeRunningCounter_) + ticks;
            if (advanced > 0xFFFFU) {
                sh2FrtRegisters_[kSh2FrtStatus - kSh2FrtBase] |=
                    kSh2FrtOverflowFlag;
            }
            sh2FreeRunningCounter_ = static_cast<std::uint16_t>(advanced);
        }
    }
    context_ = context;
    scuDma_.setExecutionContext(context.step, context.blockAddress,
                                context.callerBlock);
    video_.setExecutionContext(context.step, context.blockAddress,
                               context.callerBlock);
}

bool MmioInterceptor::isMmio(AddressSpace space) noexcept {
    switch (space) {
    case AddressSpace::Smpc:
    case AddressSpace::Vdp1:
    case AddressSpace::Vdp2:
    case AddressSpace::Scu:
    case AddressSpace::Scsp:
    case AddressSpace::CdBlock:
        return true;
    case AddressSpace::Unknown:
    case AddressSpace::Bios:
    case AddressSpace::LowWorkRam:
    case AddressSpace::HighWorkRam:
        return false;
    }
    return false;
}

std::optional<std::uint32_t> MmioInterceptor::read(core::Address address,
                                                    std::uint8_t width) {
    // Ordinary work RAM is the overwhelmingly common bus access. Let the
    // backing store handle it before checking the individual device decoders.
    const auto ramWindow = address & 0xFFF00000U;
    if (ramWindow == 0x06000000U || ramWindow == 0x00200000U) return std::nullopt;
    if (isSh2FrtRegister(address, width)) {
        std::uint32_t value{};
        for (std::uint8_t offset = 0U; offset < width; ++offset) {
            const auto current = static_cast<core::Address>(address + offset);
            const auto byte = current == kSh2FrtCounterHigh
                ? static_cast<std::uint8_t>(sh2FreeRunningCounter_ >> 8U)
                : current == kSh2FrtCounterLow
                    ? static_cast<std::uint8_t>(sh2FreeRunningCounter_)
                    : sh2FrtRegisters_[current - kSh2FrtBase];
            value = static_cast<std::uint32_t>((value << 8U) | byte);
        }
        if (accessRecording_) {
            recordAccess(address, width, value, AddressSpace::Unknown, false);
        }
        return value;
    }
    if (isSh2DivisionRegister(address)) {
        std::uint32_t value{};
        switch (address) {
        case kSh2DivisionDivisor:
            value = sh2DivisionDivisor_;
            break;
        case kSh2DivisionDividendLow:
            value = sh2DivisionDividendLow_;
            break;
        case kSh2DivisionDividendHigh:
            value = sh2DivisionDividendHigh_;
            break;
        case kSh2DivisionControl:
            value = sh2DivisionControl_;
            break;
        case kSh2DivisionQuotient:
            if (sh2DivisionDivisor_ == 0U) {
                value = 0xFFFFFFFFU;
            } else {
                const auto dividend = (static_cast<std::uint64_t>(sh2DivisionDividendHigh_) << 32U) |
                                      sh2DivisionDividendLow_;
                value = static_cast<std::uint32_t>(dividend / sh2DivisionDivisor_);
            }
            break;
        default:
            if (const auto found = registers_.find({address, width});
                found != registers_.end()) {
                value = found->second;
            }
            break;
        }
        value &= widthMask(width);
        if (accessRecording_) {
            recordAccess(address, width, value, AddressSpace::Unknown, false);
        }
        return value;
    }
    if (isSh2DmaRegister(address)) {
        std::uint32_t value{};
        switch (address) {
        case kSh2DmaSar0: value = sh2DmaSar0_; break;
        case kSh2DmaDar0: value = sh2DmaDar0_; break;
        case kSh2DmaTcr0: value = sh2DmaTcr0_; break;
        case kSh2DmaChcr0: value = sh2DmaChcr0_; break;
        case kSh2DmaSar1: value = sh2DmaSar1_; break;
        case kSh2DmaDar1: value = sh2DmaDar1_; break;
        case kSh2DmaTcr1: value = sh2DmaTcr1_; break;
        case kSh2DmaChcr1: value = sh2DmaChcr1_; break;
        case kSh2DmaVcr0: value = sh2DmaVcr0_; break;
        case kSh2DmaVcr1: value = sh2DmaVcr1_; break;
        case kSh2DmaOr: value = sh2DmaOr_; break;
        default: break;
        }
        value &= widthMask(width);
        if (accessRecording_) {
            recordAccess(address, width, value, AddressSpace::Unknown, false);
        }
        return value;
    }
    const auto space = classify(address);
    if (!isMmio(space)) {
        return std::nullopt;
    }
    const auto mappedAddress = deviceAddress(space, address);
    std::uint32_t value{};
    if (space == AddressSpace::Scu) {
        // Timer mode shares 0x098 with an older synthetic DMA-start alias in
        // ScuDmaRuntime. Decode the real SCU timer registers first so a game
        // write cannot accidentally request DMA channel 0.
        if (mappedAddress == kScuTimer0CompareRegister) {
            value = scuTimer0Compare_;
        } else if (mappedAddress == kScuTimerModeRegister) {
            value = scuTimerMode_;
        } else if (const auto dmaValue = scuDma_.read(mappedAddress, width)) {
            value = *dmaValue;
        } else if (mappedAddress == kScuInterruptMask && width == 4U) {
            value = scuInterruptMask_;
        } else if (mappedAddress == kScuInterruptStatus && width == 4U) {
            value = scuInterruptStatus_;
        } else if (mappedAddress == kScuAbusAcknowledge && width == 4U) {
            value = abusInterruptAcknowledge_;
        } else if (const auto found = registers_.find({address, width});
                   found != registers_.end()) {
            value = found->second;
        }
    } else if (const auto found = registers_.find({address, width}); found != registers_.end()) {
        value = found->second;
    } else if (space == AddressSpace::Smpc) {
        value = smpc_.read(mappedAddress, width).value_or(0U);
    } else if (space == AddressSpace::CdBlock) {
        value = cd_.read(mappedAddress, width).value_or(0U);
    } else if (space == AddressSpace::Vdp1 || space == AddressSpace::Vdp2) {
        value = video_.read(mappedAddress, width).value_or(0U);
    } else if (space == AddressSpace::Scsp) {
        value = scsp_.read(mappedAddress, width).value_or(0U);
    } else {
        const auto fallbackRegister = registers_.find({address, width});
        value = fallbackRegister == registers_.end() ? 0U : fallbackRegister->second;
    }
    recordControlAccess(address, width, value, space, false);
    if (accessRecording_) recordAccess(address, width, value, space, false);
    return value;
}

bool MmioInterceptor::write(core::Address address, std::uint8_t width,
                            std::uint32_t value) {
    const auto ramWindow = address & 0xFFF00000U;
    if (ramWindow == 0x06000000U || ramWindow == 0x00200000U) return false;
    if ((address == 0x01000000U || address == 0x21000000U) && width == 2U) {
        slaveInputCapture_ = true;
        return true;
    }
    if (isSh2FrtRegister(address, width)) {
        value &= widthMask(width);
        for (std::uint8_t offset = 0U; offset < width; ++offset) {
            const auto shift = static_cast<unsigned>((width - offset - 1U) * 8U);
            const auto byte = static_cast<std::uint8_t>(value >> shift);
            const auto current = static_cast<core::Address>(address + offset);
            if (current == kSh2FrtCounterHigh) {
                sh2FreeRunningCounter_ = static_cast<std::uint16_t>(
                    (sh2FreeRunningCounter_ & 0x00FFU) |
                    (static_cast<std::uint16_t>(byte) << 8U));
            } else if (current == kSh2FrtCounterLow) {
                sh2FreeRunningCounter_ = static_cast<std::uint16_t>(
                    (sh2FreeRunningCounter_ & 0xFF00U) | byte);
            } else {
                sh2FrtRegisters_[current - kSh2FrtBase] = byte;
                if (current == kSh2FrtControl) {
                    sh2FrtBlockRemainder_ = 0U;
                }
            }
        }
        if (accessRecording_) {
            recordAccess(address, width, value, AddressSpace::Unknown, true);
        }
        return true;
    }
    if (isSh2DivisionRegister(address)) {
        value &= widthMask(width);
        switch (address) {
        case kSh2DivisionDivisor:
            sh2DivisionDivisor_ = value;
            break;
        case kSh2DivisionDividendLow:
            sh2DivisionDividendLow_ = value;
            break;
        case kSh2DivisionDividendHigh:
            sh2DivisionDividendHigh_ = value;
            break;
        case kSh2DivisionControl:
            sh2DivisionControl_ = value;
            break;
        case kSh2DivisionQuotient:
            // The quotient is read-only from the SH-2's point of view.
            break;
        default:
            registers_.insert_or_assign({address, width}, value);
            break;
        }
        if (accessRecording_) {
            recordAccess(address, width, value, AddressSpace::Unknown, true);
        }
        return true;
    }
    if (isSh2DmaRegister(address)) {
        value &= widthMask(width);
        // The ROM/game writes these as 32-bit registers. Preserve the
        // existing upper bits for narrower accesses, matching the simple
        // byte/word-visible behavior needed by the Saturn runtime.
        auto assign = [width, value](std::uint32_t& target) {
            if (width == 4U) target = value;
            else if (width == 2U) target = (target & 0xFFFF0000U) | value;
            else target = (target & 0xFFFFFF00U) | value;
        };
        switch (address) {
        case kSh2DmaSar0: assign(sh2DmaSar0_); break;
        case kSh2DmaDar0: assign(sh2DmaDar0_); break;
        case kSh2DmaTcr0: sh2DmaTcr0_ = value & 0x00FFFFFFU; break;
        case kSh2DmaChcr0: assign(sh2DmaChcr0_); break;
        case kSh2DmaSar1: assign(sh2DmaSar1_); break;
        case kSh2DmaDar1: assign(sh2DmaDar1_); break;
        case kSh2DmaTcr1: sh2DmaTcr1_ = value & 0x00FFFFFFU; break;
        case kSh2DmaChcr1: assign(sh2DmaChcr1_); break;
        case kSh2DmaVcr0: sh2DmaVcr0_ = value & 0xFFFFU; break;
        case kSh2DmaVcr1: sh2DmaVcr1_ = value & 0xFFFFU; break;
        case kSh2DmaOr: sh2DmaOr_ = value & 0xFU; break;
        default: break;
        }
        if (accessRecording_) {
            recordAccess(address, width, value, AddressSpace::Unknown, true);
        }
        return true;
    }
    const auto space = classify(address);
    if (!isMmio(space)) {
        return false;
    }
    const auto mappedAddress = deviceAddress(space, address);
    const auto mask = widthMask(width);
    value &= mask;
    if (space == AddressSpace::Smpc) {
        static_cast<void>(smpc_.write(mappedAddress, width, value));
        scsp_.setSoundCpuEnabled(smpc_.soundCpuEnabled());
    } else if (space == AddressSpace::CdBlock) {
        recordControlAccess(mappedAddress, width, value, space, true);
        static_cast<void>(cd_.write(mappedAddress, width, value));
        scuDma_.onCdStateChanged();
    } else if (space == AddressSpace::Vdp1 || space == AddressSpace::Vdp2) {
        if (space == AddressSpace::Vdp1 && video_.vdp1MemoryWriteTraceEnabled() &&
            mappedAddress >= VideoRuntime::kVdp1Base &&
            mappedAddress < VideoRuntime::kVdp1Base + VideoRuntime::kVdp1VramSize) {
            const auto offset = static_cast<std::size_t>(
                mappedAddress - VideoRuntime::kVdp1Base);
            if (offset < 0x60300U && offset + width > 0x60000U) {
                std::array<std::uint32_t, 8> registers{};
                const bool registersAvailable = machineState_ != nullptr ||
                                                !context_.state.empty();
                for (std::uint8_t index = 0U; index < registers.size(); ++index) {
                    const auto slot = sh2::stateSlot(static_cast<sh2::Register>(index));
                    if (machineState_) {
                        registers[index] = machineState_->readState(slot);
                    } else if (const auto valueAtSlot = context_.state.find(slot);
                               valueAtSlot != context_.state.end()) {
                        registers[index] = valueAtSlot->second;
                    }
                }
                video_.setVdp1MemoryWriteRegisters(registers, registersAvailable);
            }
        }
        static_cast<void>(video_.write(mappedAddress, width, value));
    } else if (space == AddressSpace::Scsp) {
        static_cast<void>(scsp_.write(mappedAddress, width, value));
    } else if (space == AddressSpace::Scu) {
        const auto timerHandled = mappedAddress == kScuTimer0CompareRegister ||
                                  mappedAddress == kScuTimerModeRegister;
        const auto dmaHandled = timerHandled ? false :
            scuDma_.write(mappedAddress, width, value);
        if (mappedAddress == kScuTimer0CompareRegister) {
            scuTimer0Compare_ = static_cast<std::uint16_t>(value & 0x03FFU);
        } else if (mappedAddress == kScuTimerModeRegister) {
            scuTimerMode_ = value;
        } else if (!dmaHandled && mappedAddress == kScuInterruptMask && width == 4U) {
            scuInterruptMask_ = value;
        } else if (!dmaHandled && mappedAddress == kScuInterruptStatus && width == 4U) {
            // IST is a read-only status register on the Saturn.  The BIOS
            // mask helpers may touch the mapped address while changing IMS,
            // but those writes must not discard a completion that is already
            // pending; the event source (or an explicit acknowledge path)
            // clears the factor instead.
            static_cast<void>(value);
        } else if (!dmaHandled && mappedAddress == kScuAbusAcknowledge && width == 4U) {
            abusInterruptAcknowledge_ = value;
        } else if (!dmaHandled) {
            registers_.insert_or_assign({address, width}, value);
        }
    } else {
        registers_.insert_or_assign({address, width}, value);
    }
    if (space == AddressSpace::Smpc) {
        recordControlAccess(address, width, value, space, true);
    }
    if (accessRecording_) recordAccess(address, width, value, space, true);
    return true;
}

void MmioInterceptor::setReadValue(core::Address address, std::uint8_t width,
                                   std::uint32_t value) {
    const auto space = classify(address);
    const auto mappedAddress = deviceAddress(space, address);
    if (space == AddressSpace::Scu && mappedAddress == kScuInterruptMask && width == 4U) {
        scuInterruptMask_ = value;
        return;
    }
    if (space == AddressSpace::Scu && mappedAddress == kScuInterruptStatus && width == 4U) {
        scuInterruptStatus_ = value;
        return;
    }
    if (space == AddressSpace::Scu && mappedAddress == kScuAbusAcknowledge && width == 4U) {
        abusInterruptAcknowledge_ = value;
        return;
    }
    if (space == AddressSpace::Scu && mappedAddress == kScuTimer0CompareRegister) {
        scuTimer0Compare_ = static_cast<std::uint16_t>(value & 0x03FFU);
        return;
    }
    if (space == AddressSpace::Scu && mappedAddress == kScuTimerModeRegister) {
        scuTimerMode_ = value;
        return;
    }
    registers_.insert_or_assign({address, width}, value);
}

void MmioInterceptor::clearAccesses() {
    accesses_.clear();
    accessCount_ = 0U;
}

void MmioInterceptor::setAccessHistoryLimit(std::size_t limit) noexcept {
    accessHistoryLimit_ = limit;
    while (accesses_.size() > accessHistoryLimit_) accesses_.pop_front();
}

void MmioInterceptor::recordAccess(core::Address address, std::uint8_t width,
                                   std::uint32_t value, AddressSpace space,
                                   bool write) {
    ++accessCount_;
    if (accessHistoryLimit_ == 0U) return;
    if (accesses_.size() == accessHistoryLimit_) accesses_.pop_front();
    accesses_.push_back({address, width, value, space, write, context_});
}

void MmioInterceptor::setAccessRecording(bool enabled) noexcept {
    accessRecording_ = enabled;
    if (!enabled) clearAccesses();
}

const std::deque<MmioAccess>& MmioInterceptor::accesses() const noexcept {
    return accesses_;
}

std::size_t MmioInterceptor::accessCount() const noexcept { return accessCount_; }

void MmioInterceptor::setControlTraceEnabled(bool enabled) noexcept {
    controlTraceEnabled_ = enabled;
    if (!enabled) {
        controlTrace_.clear();
        controlTraceSequence_ = 0U;
        droppedControlTraceRecords_ = 0U;
    }
}

void MmioInterceptor::setControlTraceStartStep(std::size_t step) noexcept {
    controlTraceStartStep_ = step;
}

void MmioInterceptor::setControlTraceOreg2Only(bool enabled) noexcept {
    controlTraceOreg2Only_ = enabled;
}

void MmioInterceptor::appendControlTrace(ControlTraceRecord record) {
    if (!controlTraceEnabled_) return;
    record.sequence = ++controlTraceSequence_;
    if (controlTrace_.size() == kControlTraceHistoryLimit) {
        controlTrace_.pop_front();
        ++droppedControlTraceRecords_;
    }
    controlTrace_.push_back(record);
}

void MmioInterceptor::recordPadInput(std::size_t step, std::size_t port,
                                     const DigitalPadState& state) {
    if (!controlTraceEnabled_ || step < controlTraceStartStep_) return;
    ControlTraceRecord record{};
    record.step = step;
    record.value = state.pressed;
    record.kind = ControlTraceKind::PadInput;
    record.port = static_cast<std::uint8_t>(port);
    record.connected = state.connected;
    appendControlTrace(record);
}

void MmioInterceptor::recordControlAccess(core::Address address,
                                          std::uint8_t width,
                                          std::uint32_t value,
                                          AddressSpace space,
                                          bool write) {
    if (!controlTraceEnabled_ ||
        (space != AddressSpace::Smpc && space != AddressSpace::CdBlock)) {
        return;
    }
    ControlTraceRecord record{};
    record.step = context_.step;
    if (record.step < controlTraceStartStep_) return;
    record.block = context_.blockAddress;
    record.caller = context_.callerBlock;
    record.address = address;
    record.width = width;
    record.value = value;
    if (space == AddressSpace::Smpc) {
        if (controlTraceOreg2Only_ &&
            (write || address != kSmpcOreg2)) {
            return;
        }
        record.kind = write ? ControlTraceKind::SmpcWrite :
                              ControlTraceKind::SmpcRead;
    } else {
        if (!write || address != CdRuntime::kCommandBase + 0x24U ||
            (cd_.commandRegister(0U) >> 8U) == 0U) {
            return; // status polling is high-volume; retain submitted commands.
        }
        record.kind = ControlTraceKind::CdCommand;
        for (std::size_t index = 0; index < record.cdRegisters.size() - 1U; ++index) {
            record.cdRegisters[index] = cd_.commandRegister(index);
        }
        record.cdRegisters[3] = static_cast<std::uint16_t>(value);
    }
    appendControlTrace(record);
}

const std::deque<ControlTraceRecord>&
MmioInterceptor::controlTrace() const noexcept {
    return controlTrace_;
}

std::size_t MmioInterceptor::droppedControlTraceRecords() const noexcept {
    return droppedControlTraceRecords_;
}

SmpcRuntime& MmioInterceptor::smpc() noexcept { return smpc_; }
const SmpcRuntime& MmioInterceptor::smpc() const noexcept { return smpc_; }
CdRuntime& MmioInterceptor::cd() noexcept { return cd_; }
const CdRuntime& MmioInterceptor::cd() const noexcept { return cd_; }
VideoRuntime& MmioInterceptor::video() noexcept { return video_; }
const VideoRuntime& MmioInterceptor::video() const noexcept { return video_; }
ScuDmaRuntime& MmioInterceptor::scuDma() noexcept { return scuDma_; }
const ScuDmaRuntime& MmioInterceptor::scuDma() const noexcept { return scuDma_; }
ScspRuntime& MmioInterceptor::scsp() noexcept { return scsp_; }
const ScspRuntime& MmioInterceptor::scsp() const noexcept { return scsp_; }

void MmioInterceptor::attachMachineState(sir::MachineState& state) noexcept {
    machineState_ = &state;
    scuDma_.attach(state, cd_);
}

void MmioInterceptor::captureFrtInput() noexcept {
    sh2FrtRegisters_[kSh2FrtStatus - kSh2FrtBase] |= 0x80U;
}

bool MmioInterceptor::takeSlaveInputCapture() noexcept {
    const auto pending = slaveInputCapture_;
    slaveInputCapture_ = false;
    return pending;
}

void MmioInterceptor::onSchedulerTick(sir::MachineState& state) {
    // The SH-2 DMA unit is clocked independently from SCU DMA.  Rockman's
    // startup uses channel 0 to copy a short work-RAM command buffer, then
    // polls CHCR.TE through 0xFFFFFF8C.  Complete enabled transfers at this
    // deterministic scheduler boundary so that poll observes the same TE
    // transition without making the interpreter execute each bus cycle.
    if ((sh2DmaOr_ & 0x7U) != 0x1U) {
        return;
    }
    auto service = [&state](std::uint32_t& sar, std::uint32_t& dar,
                            std::uint32_t& tcr, std::uint32_t& chcr) {
        if ((chcr & 0x1U) == 0U || (chcr & 0x2U) != 0U || tcr == 0U) {
            return;
        }
        const auto sizeCode = static_cast<unsigned>((chcr >> 10U) & 0x3U);
        const auto width = sizeCode == 0U ? 1U : sizeCode == 1U ? 2U : 4U;
        const auto sourceMode = static_cast<unsigned>((chcr >> 12U) & 0x3U);
        const auto destinationMode = static_cast<unsigned>((chcr >> 14U) & 0x3U);
        const auto delta = static_cast<std::int32_t>(width);
        const auto sourceDelta = sourceMode == 1U ? delta :
            sourceMode == 2U ? -delta : 0;
        const auto destinationDelta = destinationMode == 1U ? delta :
            destinationMode == 2U ? -delta : 0;
        auto count = tcr & 0x00FFFFFFU;
        while (count-- != 0U) {
            if (width == 1U) {
                state.write8(dar, state.read8(sar));
            } else if (width == 2U) {
                state.write16BE(dar, state.read16BE(sar));
            } else {
                state.write32BE(dar, state.read32BE(sar));
            }
            sar = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(sar) + sourceDelta);
            dar = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(dar) + destinationDelta);
        }
        tcr = 0U;
        chcr |= 0x2U; // transfer-end status (TE)
    };
    service(sh2DmaSar0_, sh2DmaDar0_, sh2DmaTcr0_, sh2DmaChcr0_);
    service(sh2DmaSar1_, sh2DmaDar1_, sh2DmaTcr1_, sh2DmaChcr1_);
}

void MmioInterceptor::raiseScuInterrupt(std::uint32_t factor) noexcept {
    scuInterruptStatus_ |= factor;
}

void MmioInterceptor::clearScuInterrupt(std::uint32_t factor) noexcept {
    scuInterruptStatus_ &= ~factor;
}

std::uint32_t MmioInterceptor::scuInterruptStatus() const noexcept {
    return scuInterruptStatus_;
}

std::uint32_t MmioInterceptor::scuInterruptMask() const noexcept {
    return scuInterruptMask_;
}

std::uint32_t MmioInterceptor::scuInterruptPending() const noexcept {
    return scuInterruptStatus_ & ~scuInterruptMask_;
}

std::uint16_t MmioInterceptor::scuTimer0Compare() const noexcept {
    return scuTimer0Compare_;
}

bool MmioInterceptor::scuTimer0Enabled() const noexcept {
    return (scuTimerMode_ & 0x00000100U) != 0U;
}

} // namespace saturn::platform
