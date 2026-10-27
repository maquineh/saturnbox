#include "saturn/saturn/scheduler.hpp"

#include <algorithm>
#include <bit>

namespace saturn::platform {

ScuEventScheduler::ScuEventScheduler(std::size_t vblankPeriodBlocks) noexcept
    : vblankPeriodBlocks_(vblankPeriodBlocks),
      hblankPeriodBlocks_(vblankPeriodBlocks == 0U ? 0U :
          std::max<std::size_t>(1U, vblankPeriodBlocks / kScanlinesPerFrame)),
      cdSubcodePeriodBlocks_(vblankPeriodBlocks == 0U ? 0U :
          std::max<std::size_t>(1U, vblankPeriodBlocks * 4U / 5U)) {}

void ScuEventScheduler::setVBlankPeriod(std::size_t periodBlocks) noexcept {
    vblankPeriodBlocks_ = periodBlocks;
    periodCounter_ = 0U;
    hblankPeriodBlocks_ = periodBlocks == 0U ? 0U :
        std::max<std::size_t>(1U, periodBlocks / kScanlinesPerFrame);
    hblankCounter_ = 0U;
    cdSubcodePeriodBlocks_ = periodBlocks == 0U ? 0U :
        std::max<std::size_t>(1U, periodBlocks * 4U / 5U);
    cdSubcodeCounter_ = 0U;
    reportedMasked_ = false;
}

std::size_t ScuEventScheduler::vblankPeriod() const noexcept {
    return vblankPeriodBlocks_;
}

void ScuEventScheduler::requestVBlank() noexcept {
    requestedVBlank_ = true;
}

std::optional<sir::InterruptRequest> ScuEventScheduler::beforeBlock(
    const sir::BasicBlock& block, sir::MachineState& state, std::size_t step,
    MmioInterceptor& mmio) {
    static_cast<void>(block);
    const auto raiseVblank = [&] {
        mmio.smpc().onVBlank();
        // SCU DMA channels whose activation factor is VBlank-IN are started
        // at the same edge as the interrupt factor is raised.
        mmio.scuDma().onVBlank();
        mmio.raiseScuInterrupt(kVBlankInStatus);
        // A VBlank is the presentation boundary for the HLE video runtime.
        // Advance it even when the SH-2 masks or drops the corresponding
        // interrupt; masking affects CPU delivery, not scan-out progress.
        mmio.scsp().onVBlank();
        mmio.video().advanceFrame();
        mmio.video().presentFrame();
        scanlineCounter_ = 0U;
        if (hblankPeriodBlocks_ != 0U) {
            vblankOutCountdown_ = hblankPeriodBlocks_ * kVBlankScanlines;
        }
        ++stats_.vblankEvents;
    };
    const auto raiseVblankOut = [&] {
        mmio.scuDma().onActivationFactor(
            ScuDmaRuntime::ActivationFactor::VBlankOut);
        mmio.raiseScuInterrupt(kVBlankOutStatus);
        ++stats_.vblankOutEvents;
        // Sega specifies T0C=0 as the V-Blank-OUT timing point.
        if (mmio.scuTimer0Enabled() && mmio.scuTimer0Compare() == 0U) {
            mmio.scuDma().onActivationFactor(
                ScuDmaRuntime::ActivationFactor::Timer0);
            mmio.raiseScuInterrupt(kTimer0Status);
            ++stats_.timer0Events;
        }
    };
    const auto raiseHblank = [&] {
        // HBlank-IN is a distinct SCU factor (bit 2/vector 0x42). Games use
        // it for scanline-side bookkeeping even before the first VDP1 list
        // is started, so it must not be inferred from PTMR or draw-end.
        mmio.scuDma().onActivationFactor(
            ScuDmaRuntime::ActivationFactor::HBlankIn);
        mmio.raiseScuInterrupt(kHBlankInStatus);
        ++stats_.hblankEvents;
        // T0C is a 1-based H-Blank line count through 263. Zero is a special
        // case handled only at V-Blank-OUT above; do not raise it again when
        // the line counter wraps.
        scanlineCounter_ = scanlineCounter_ < kScanlinesPerFrame
            ? scanlineCounter_ + 1U : 1U;
        const auto timer0Compare = mmio.scuTimer0Compare();
        if (mmio.scuTimer0Enabled() && timer0Compare != 0U &&
            scanlineCounter_ == timer0Compare) {
            mmio.scuDma().onActivationFactor(
                ScuDmaRuntime::ActivationFactor::Timer0);
            mmio.raiseScuInterrupt(kTimer0Status);
            ++stats_.timer0Events;
        }
    };

    // Interrupt entry is checked more than once for a block when a handler is
    // entered. Count elapsed time only when ProgramExecutor advances its block
    // step, so a period is independent of handler depth.
    if (!hasStep_ || step != lastStep_) {
        hasStep_ = true;
        lastStep_ = step;
        ++stats_.elapsedBlocks;
        if (vblankPeriodBlocks_ != 0U && ++periodCounter_ >= vblankPeriodBlocks_) {
            periodCounter_ = 0U;
            raiseVblank();
        }
        if (hblankPeriodBlocks_ != 0U && ++hblankCounter_ >= hblankPeriodBlocks_) {
            hblankCounter_ = 0U;
            raiseHblank();
        }
        // CD subcode frames are produced at 75 Hz. Derive their deterministic
        // cadence from the configured ~60 Hz presentation period, but keep
        // them independent of VBlank so consecutive pause/seek waits cannot
        // consume the same single edge.
        if (cdSubcodePeriodBlocks_ != 0U &&
            ++cdSubcodeCounter_ >= cdSubcodePeriodBlocks_) {
            cdSubcodeCounter_ = 0U;
            mmio.cd().onSubcodeTick();
            // A subcode tick may have published a retained sector into the CD
            // FIFO. Give level-7 SCU DMA its activation edge immediately so
            // the transfer completes before the next foreground poll.
            mmio.scuDma().onCdStateChanged();
            ++stats_.cdSubcodeEvents;
        }
        if (vblankOutCountdown_ != 0U && --vblankOutCountdown_ == 0U) {
            raiseVblankOut();
        }
    }
    if (requestedVBlank_) {
        requestedVBlank_ = false;
        raiseVblank();
    }

    // The 68K-side SCSP producer is modeled as a deterministic mailbox
    // service. It runs after the preceding block has had a chance to publish
    // its request words and before the next block observes the ready flags.
    mmio.scsp().onSchedulerTick(state);
    mmio.onSchedulerTick(state);

    // The HLE renderer completes a PTMR=1 manual list synchronously and a
    // PTMR=2 automatic list at VBlank above. In both cases the SCU draw-end
    // factor is sampled only at this boundary, so it cannot pre-empt the
    // initiating block.
    if (mmio.video().takeVdp1DrawEnd()) {
        mmio.scuDma().onActivationFactor(
            ScuDmaRuntime::ActivationFactor::SpriteDrawEnd);
        mmio.raiseScuInterrupt(kSpriteDrawEndStatus);
    }

    // INTBACK completion publishes OREGs and requests SCU vector 0x47.
    // Consume the device edge even while masked; SCU retains the factor.
    if (mmio.smpc().takeInterruptRequest()) mmio.raiseScuInterrupt(1U << 7U);
    // A completed SCU DMA transfer raises its level-specific interrupt
    // factor.  Completion is sampled here rather than from the DMA write
    // path so an interrupt cannot pre-empt the block that performed the last
    // transfer word.
    const auto completedDma = mmio.scuDma().takeCompletionMask();
    // Channel 0 is SCU level-0 DMA (vector 0x4B), channel 1 level-1
    // (0x4A), and channel 2 level-2 (0x49).
    if ((completedDma & 0x1U) != 0U) mmio.raiseScuInterrupt(kLevel0DmaEndStatus);
    if ((completedDma & 0x2U) != 0U) mmio.raiseScuInterrupt(kLevel1DmaEndStatus);
    if ((completedDma & 0x4U) != 0U) mmio.raiseScuInterrupt(kLevel2DmaEndStatus);

    const auto status = mmio.scuInterruptStatus();
    const auto pending = mmio.scuInterruptPending();
    if (status != 0U && pending == 0U) {
        if (!reportedMasked_) {
            ++stats_.maskedEvents;
            reportedMasked_ = true;
        }
        return std::nullopt;
    }
    if (pending == 0U) {
        reportedMasked_ = false;
        return std::nullopt;
    }

    constexpr std::uint32_t kSrInterruptMask = 0x000000F0U;
    constexpr std::uint32_t kSrBlock = 0x10000000U;
    const auto sr = state.readState(sir::kStatusRegisterStateSlot);
    if ((sr & kSrBlock) != 0U || ((sr & kSrInterruptMask) >> 4U) >= 15U) {
        // Keep one diagnostic count per coalesced factor. The handler may
        // spin for thousands of blocks with BL/IMASK asserted, but hardware
        // still exposes one pending VBlank factor until it is acknowledged.
        if (!reportedMasked_) {
            ++stats_.maskedEvents;
            reportedMasked_ = true;
        }
        return std::nullopt;
    }

    const auto factor = static_cast<std::uint32_t>(
        std::countr_zero(pending));
    const auto vector = static_cast<std::uint8_t>(0x40U + factor);
    const auto vbr = state.readState(sir::kVectorBaseStateSlot);
    const auto vectorOffset = static_cast<core::Address>(vector * 4U);
    const auto vectorTarget = state.read32BE(vbr + vectorOffset);
    const auto scuTarget = state.read32BE(kScuInterruptTable + vectorOffset);
    const auto hasScuCallback = scuTarget != 0U &&
        scuTarget != kDummyInterruptHandler;
    auto target = hasScuCallback ? scuTarget : vectorTarget;
    auto returnMode = hasScuCallback ? sir::InterruptReturnMode::Callback
                                     : sir::InterruptReturnMode::Exception;
    // SYS_SETUINT installs the BIOS vector trampoline in both tables during
    // boot.  The trampoline indexes the application table and would call the
    // exact same address again while its entry still points at itself.  A
    // native Saturn returns through the BIOS wrapper in this case; consume
    // the factor and leave the CPU on the interrupted block until a concrete
    // callback replaces the table entry.
    if (hasScuCallback) {
        const auto callbackEntry = state.read32BE(
            kApplicationDispatchTable + vectorOffset);
        if (callbackEntry == target) {
            target = vectorTarget;
            returnMode = sir::InterruptReturnMode::Exception;
        }
    }
    mmio.clearScuInterrupt(static_cast<std::uint32_t>(1U << factor));
    reportedMasked_ = false;
    if (target == 0U || target == kDummyInterruptHandler) {
        ++stats_.droppedEvents;
        return std::nullopt;
    }

    ++stats_.deliveredInterrupts;
    return sir::InterruptRequest{target, returnMode};
}

const ScuEventScheduler::Stats& ScuEventScheduler::stats() const noexcept {
    return stats_;
}

} // namespace saturn::platform
