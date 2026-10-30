#include "saturn/saturn/scheduler.hpp"

#include <algorithm>
#include <bit>
#include <chrono>

namespace saturn::platform {
namespace {

using ProfileClock = std::chrono::steady_clock;

[[nodiscard]] bool shouldSample(ScuEventScheduler::Stats::TimingProfile& profile) noexcept {
    ++profile.calls;
    return profile.calls % ScuEventScheduler::kProfileSampleInterval == 0U;
}

void recordSample(ScuEventScheduler::Stats::TimingProfile& profile,
                  ProfileClock::time_point start) noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        ProfileClock::now() - start).count();
    profile.sampledNanoseconds += static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed));
    ++profile.sampledCalls;
    profile.estimatedNanoseconds =
        (profile.sampledNanoseconds / profile.sampledCalls) * profile.calls;
}

} // namespace

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

void ScuEventScheduler::setProfilingEnabled(bool enabled) noexcept {
    if (profilingEnabled_ == enabled) return;
    profilingEnabled_ = enabled;
    stats_.scspBlockTick = {};
    stats_.scspSchedulerTick = {};
    stats_.mmioSchedulerTick = {};
    stats_.scspVBlank = {};
    stats_.videoVBlankOut = {};
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
        // The IN handler may request VBE erase/change during this blank.
        // Finish it at OUT, before the guest clears VBE in its OUT handler.
        if (profilingEnabled_) {
            auto& profile = stats_.scspVBlank;
            ++profile.calls;
            const auto start = ProfileClock::now();
            mmio.scsp().onVBlank();
            recordSample(profile, start);
        } else {
            mmio.scsp().onVBlank();
        }
        mmio.video().onVBlankIn();
        scanlineCounter_ = 0U;
        // Explicit events (period zero) still need an OUT edge. Artificial
        // periods shorter than blank use a bounded one-block approximation.
        vblankOutCountdown_ = hblankPeriodBlocks_ == 0U ? 1U :
            std::min(hblankPeriodBlocks_ * kVBlankScanlines,
                     std::max<std::size_t>(1U, vblankPeriodBlocks_ - 1U));
        ++stats_.vblankEvents;
    };
    const auto raiseVblankOut = [&] {
        if (profilingEnabled_) {
            auto& profile = stats_.videoVBlankOut;
            ++profile.calls;
            const auto start = ProfileClock::now();
            mmio.video().onVBlankOut();
            recordSample(profile, start);
        } else {
            mmio.video().onVBlankOut();
        }
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
        // Sound time advances with the same quantum as VBlank, so sound-CPU
        // execution and SH-2 register writes interleave inside a frame
        // instead of only at the frame boundary.
        mmio.scsp().setAudioFrameBlocks(vblankPeriodBlocks_);
        if (profilingEnabled_) {
            auto& profile = stats_.scspBlockTick;
            if (shouldSample(profile)) {
                const auto start = ProfileClock::now();
                mmio.scsp().onBlockTick();
                recordSample(profile, start);
            } else {
                mmio.scsp().onBlockTick();
            }
        } else {
            mmio.scsp().onBlockTick();
        }
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

    // SCSP command servicing is independent from the game's VDP1 field-sync
    // mailbox. Keep its audio/driver work at block boundaries, then enable the
    // legacy mailbox approximation only in harnesses with no VBlank clock.
    if (profilingEnabled_) {
        auto& scspProfile = stats_.scspSchedulerTick;
        if (shouldSample(scspProfile)) {
            const auto start = ProfileClock::now();
            mmio.scsp().onSchedulerTick();
            recordSample(scspProfile, start);
        } else {
            mmio.scsp().onSchedulerTick();
        }
        auto& mmioProfile = stats_.mmioSchedulerTick;
        if (shouldSample(mmioProfile)) {
            const auto start = ProfileClock::now();
            mmio.onSchedulerTick(state);
            recordSample(mmioProfile, start);
        } else {
            mmio.onSchedulerTick(state);
        }
    } else {
        mmio.scsp().onSchedulerTick();
        mmio.onSchedulerTick(state);
    }
    if (vblankPeriodBlocks_ == 0U)
        mmio.scsp().serviceFramebufferMailboxFallback(state);

    // The HLE renderer completes a PTMR=1 manual list synchronously and a
    // PTMR=2 automatic list at VBlank-OUT above. In both cases the SCU draw-end
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
