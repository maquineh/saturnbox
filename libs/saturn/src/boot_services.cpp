#include "saturn/saturn/boot_services.hpp"

#include "saturn/sh2/module_lifter.hpp"

#include <string>

namespace saturn::platform {

BootRomServices::BootRomServices()
    : slots_{{kExecuteDmpPointer, kExitStubAddress, "SYS_EXECDMP"},
             {kCdInit2Pointer, kStubAddress, "SYS_CDINIT2"},
             {kCdInit1Pointer, kCdInit1StubAddress, "SYS_CDINIT1"},
             {kAipDispatchPointer, kAipDispatchStubAddress, "SYS_SETUINT"},
             {kAipGetDispatchPointer, kAipGetDispatchStubAddress, "SYS_GETUINT"},
             {kAipSetSh2DispatchPointer, kAipSetSh2DispatchStubAddress, "SYS_SETSINT"},
             {kAipGetSh2DispatchPointer, kAipGetSh2DispatchStubAddress, "SYS_GETSINT"},
             {kAipChangeSystemClockPointer, kAipChangeSystemClockStubAddress,
              "SYS_CHGSYSCK"},
             {kAipTasSemaphorePointer, kAipTasSemaphoreStubAddress, "SYS_TASSEM"},
             {kAipClearSemaphorePointer, kAipClearSemaphoreStubAddress, "SYS_CLRSEM"},
             {kAipSetScuMaskPointer, kAipSetScuMaskStubAddress, "SYS_SETSCUIM"},
             {kAipCdDispatchPointer, kAipCdDispatchStubAddress, "SYS_CHGSCUIM"},
             {kBupInitPointer, kBupInitStubAddress, "SYS_BUPINIT"}} {}

void BootRomServices::install(sir::Module& module, sir::MachineState& state) {
    state.writeState(sh2::stateSlot(sh2::Register::R15), kDefaultMasterStack);
    state.writeState(sh2::stateSlot(sh2::Register::VBR), kDefaultVectorBase);

    for (const auto& slot : slots_) {
        auto* block = module.blockAt(slot.targetAddress);
        if (!block) {
            block = &module.createBlock(slot.targetAddress,
                                        std::string("saturn.boot.service.") +
                                            std::string(slot.name));
            sir::ModuleBuilder builder(module);
            builder.setInsertPoint(*block);
            if (slot.pointerAddress == kExecuteDmpPointer) {
                // The harness keeps the historical sleep boundary by default.
                // A real BIOS execution returns to the caller through PR;
                // opt into that behavior when continuing the game runtime.
                if (executeDmpReturns_) {
                    builder.ret(builder.readState(sh2::stateSlot(sh2::Register::PR)));
                } else {
                    builder.sleep();
                }
            } else {
                builder.ret(builder.readState(sh2::stateSlot(sh2::Register::PR)));
            }
            block->markLowered();
        }
        entrypoints_.insert_or_assign(slot.targetAddress, block);
        state.write32BE(slot.pointerAddress, slot.targetAddress);
    }
    // BUPInit fills a caller-provided table with these BIOS entry points.
    // Materialize the tiny host-owned blocks at the original Saturn addresses
    // so indirect calls through that table resolve just like the BIOS switch
    // in Yabause (0x380..0x3AC).
    for (core::Address address = kBupFunctionTableFirst;
         address <= kBupFunctionTableLast; address += 4U) {
        auto* block = module.blockAt(address);
        if (!block) {
            block = &module.createBlock(
                address, "saturn.boot.bup." + std::to_string(address));
            sir::ModuleBuilder builder(module);
            builder.setInsertPoint(*block);
            builder.ret(builder.readState(sh2::stateSlot(sh2::Register::PR)));
            block->markLowered();
        }
        entrypoints_.insert_or_assign(address, block);
    }
    // BOOT ROM's SCU dispatcher starts with dummy handlers. SYS_SETUINT then
    // replaces individual entries with application callbacks.
    for (std::uint32_t vector = 0x40U; vector <= 0x5FU; ++vector) {
        state.write32BE(kScuInterruptTable + vector * 4U, kDummyInterruptHandler);
    }
    state.write32BE(kScuInterruptMaskStorage, 0xFFFFFFFFU);
    for (std::uint32_t vector = 0; vector < 0x80U; ++vector) {
        state.write32BE(kDefaultVectorBase + vector * 4U, kDummyInterruptHandler);
    }
    syncValue_ = 0;
    slaveEntryPoint_ = 0U;
    calls_.clear();
}

void BootRomServices::setExecuteDmpReturns(bool enabled) noexcept {
    executeDmpReturns_ = enabled;
}

void BootRomServices::attachMmio(MmioInterceptor& mmio) noexcept {
    mmio_ = &mmio;
}

void BootRomServices::setExecutionContext(const sir::ExecutionContext& context) {
    if (mmio_) {
        mmio_->setExecutionContext(context);
    }
    // Ordinary game blocks cannot be BIOS stubs. Keep MMIO timing above,
    // but avoid scanning the service table at every native block boundary.
    if (context.blockAddress < kStubAddress ||
        context.blockAddress > kBupInitStubAddress) return;
    for (const auto& slot : slots_) {
        if (slot.targetAddress == context.blockAddress) {
            calls_.push_back({slot, context});
            break;
        }
    }
}

bool BootRomServices::requiresStateSnapshot(core::Address address) const noexcept {
    if (address >= kStubAddress && address <= kBupInitStubAddress) {
        for (const auto& slot : slots_) {
            if (slot.targetAddress == address) return true;
        }
    }
    return mmio_ && mmio_->requiresStateSnapshot(address);
}

core::Address BootRomServices::backingAddress(core::Address address) const noexcept {
    return canonicalizeRamAddress(address);
}

void BootRomServices::executeService(const sir::BasicBlock& block,
                                     sir::MachineState& state) {
    if ((block.address() < kStubAddress || block.address() > kBupInitStubAddress) &&
        (block.address() < kBupFunctionTableFirst ||
         block.address() > kBupFunctionTableLast)) return;
    if (block.address() == kBupInitStubAddress) {
        // Mirrors Yabause's BiosBUPInit: R5 receives the function table and
        // R6 receives a six-word device descriptor list.  A Saturn with no
        // cartridge exposes the internal backup RAM as device 1 and leaves
        // the remaining two device slots empty.
        const auto table = state.readState(sh2::stateSlot(sh2::Register::R5));
        const auto devices = state.readState(sh2::stateSlot(sh2::Register::R6));
        state.write32BE(kAipCdDispatchPointer + 0x10U, table);
        for (std::uint32_t index = 0; index < 12U; ++index) {
            state.write32BE(table + index * 4U,
                            kBupFunctionTableFirst + index * 4U);
        }
        state.write16BE(devices + 0x00U, 1U);
        state.write16BE(devices + 0x02U, 1U);
        state.write16BE(devices + 0x04U, 0U);
        state.write16BE(devices + 0x06U, 0U);
        state.write16BE(devices + 0x08U, 0U);
        state.write16BE(devices + 0x0AU, 0U);
        return;
    }

    if (block.address() >= kBupFunctionTableFirst &&
        block.address() <= kBupFunctionTableLast &&
        ((block.address() - kBupFunctionTableFirst) % 4U) == 0U) {
        // Select/format/status/read/write/delete and the remaining BUP calls
        // all complete synchronously in this HLE.  Returning zero matches the
        // no-error path used by the BIOS for the initial boot queries; the
        // table and device descriptors above carry the observable state.
        state.writeState(sh2::stateSlot(sh2::Register::R0), 0U);
        return;
    }

    if (block.address() == kAipDispatchStubAddress) {
        // SYS_SETUINT(Uint32 Num, void *Hdr): a null handler means the BOOT
        // ROM dummy routine. The function has no return value and therefore
        // deliberately leaves R0 untouched.
        const auto vector = state.readState(sh2::stateSlot(sh2::Register::R4));
        const auto handler = state.readState(sh2::stateSlot(sh2::Register::R5));
        const auto address = kScuInterruptTable + vector * 4U;
        state.write32BE(address, handler == 0U ? kDummyInterruptHandler : handler);
        return;
    }

    if (block.address() == kAipChangeSystemClockStubAddress) {
        // SYS_CHGSYSCK records the requested 26/28 MHz mode in the system
        // work area. The clock itself is outside this deterministic runtime;
        // retaining the value is enough for SYS_GETSYSCK-style reads.
        state.write32BE(kSystemClockStorage,
                        state.readState(sh2::stateSlot(sh2::Register::R4)) & 1U);
        return;
    }

    if (block.address() == kAipTasSemaphoreStubAddress) {
        // SYS_TASSEM atomically tests the byte semaphore and sets its busy
        // bit. A single-threaded executor has no competing SH-2, but keeping
        // the 0x80 marker preserves the BIOS-visible state for later calls.
        const auto address = kSemaphoreBase +
            (state.readState(sh2::stateSlot(sh2::Register::R4)) & 0xFFU);
        const auto current = state.read8(address);
        state.writeState(sh2::stateSlot(sh2::Register::R0), current == 0U ? 1U : 0U);
        state.write8(address, static_cast<std::uint8_t>(current | 0x80U));
        return;
    }

    if (block.address() == kAipClearSemaphoreStubAddress) {
        const auto address = kSemaphoreBase +
            (state.readState(sh2::stateSlot(sh2::Register::R4)) & 0xFFU);
        state.write8(address, 0U);
        return;
    }

    if (block.address() == kAipSetScuMaskStubAddress) {
        const auto mask = state.readState(sh2::stateSlot(sh2::Register::R4));
        state.write32BE(kScuInterruptMaskStorage, mask);
        if (mmio_) {
            static_cast<void>(mmio_->write(kScuInterruptMaskRegister, 4, mask));
            static_cast<void>(mmio_->write(kScuInterruptStatusRegister, 4, mask));
            if ((mask & 0x8000U) == 0U) {
                static_cast<void>(mmio_->write(kAbusInterruptAcknowledge, 4, 1U));
            }
        }
        return;
    }

    if (block.address() == kAipCdDispatchStubAddress) {
        // SYS_CHGSCUIM(AndMask, OrMask) keeps a readable copy because the
        // hardware mask register itself is write-only. The mapped IST access
        // is retained for trace compatibility, while MmioInterceptor treats
        // it as read-only so pending event factors survive mask changes.
        const auto andMask = state.readState(sh2::stateSlot(sh2::Register::R4));
        const auto orMask = state.readState(sh2::stateSlot(sh2::Register::R5));
        const auto oldMask = state.read32BE(kScuInterruptMaskStorage);
        const auto newMask = (oldMask & andMask) | orMask;
        state.write32BE(kScuInterruptMaskStorage, newMask);
        if (mmio_) {
            static_cast<void>(mmio_->write(kScuInterruptMaskRegister, 4, newMask));
            static_cast<void>(mmio_->write(kScuInterruptStatusRegister, 4,
                                            andMask & ~orMask));
            if ((andMask & 0x8000U) == 0U) {
                static_cast<void>(mmio_->write(kAbusInterruptAcknowledge, 4, 1U));
            }
        }
        return;
    }

    if (block.address() == kAipGetDispatchStubAddress) {
        // SYS_GETUINT(Num) returns the registered SCU handler from the same
        // table maintained by SYS_SETUINT. The BIOS does not modify R0 on
        // entry, so the value is supplied as the normal function result.
        const auto vector = state.readState(sh2::stateSlot(sh2::Register::R4));
        state.writeState(sh2::stateSlot(sh2::Register::R0),
                         state.read32BE(kScuInterruptTable + vector * 4U));
        return;
    }

    if (block.address() == kAipSetSh2DispatchStubAddress) {
        // SYS_SETSINT(Num, Hdr) registers a handler in the SH-2 vector table;
        // a null handler restores the boot ROM dummy routine.
        const auto vector = state.readState(sh2::stateSlot(sh2::Register::R4));
        const auto handler = state.readState(sh2::stateSlot(sh2::Register::R5));
        const auto vbr = state.readState(sh2::stateSlot(sh2::Register::VBR));
        // Sega's dual-CPU boot interface uses SYS_SETSINT(0x94, entry)
        // before SSHON to replace the slave firmware entry function.
        if (vector == 0x94U) slaveEntryPoint_ = handler;
        state.write32BE(vbr + vector * 4U,
                        handler == 0U ? kDummyInterruptHandler : handler);
        return;
    }

    if (block.address() == kAipGetSh2DispatchStubAddress) {
        // SYS_GETSINT(Num) returns the handler currently installed in the
        // vector table selected by VBR.
        const auto vector = state.readState(sh2::stateSlot(sh2::Register::R4));
        const auto vbr = state.readState(sh2::stateSlot(sh2::Register::VBR));
        state.writeState(sh2::stateSlot(sh2::Register::R0),
                         state.read32BE(vbr + vector * 4U));
        return;
    }
}

std::optional<std::uint32_t> BootRomServices::read(core::Address address,
                                                   std::uint8_t width) {
    // The AIP's wait routine polls this boot-provided synchronization word.
    // A real Saturn changes it from the boot/CD service side; advancing it on
    // each observed halfword gives the single-threaded executor the same
    // observable progress without inventing a RAM write.
    if (address == kAipSyncWord && width == 2) {
        syncValue_ = static_cast<std::uint16_t>(syncValue_ + 1U);
        return syncValue_;
    }
    return mmio_ ? mmio_->read(address, width) : std::nullopt;
}

bool BootRomServices::write(core::Address address, std::uint8_t width,
                            std::uint32_t value) {
    return mmio_ && mmio_->write(address, width, value);
}

sir::BasicBlock* BootRomServices::resolve(core::Address target) const noexcept {
    const auto found = entrypoints_.find(target);
    return found == entrypoints_.end() ? nullptr : found->second;
}

const std::vector<BootServiceSlot>& BootRomServices::slots() const noexcept {
    return slots_;
}

const std::vector<BootServiceCall>& BootRomServices::calls() const noexcept {
    return calls_;
}

void BootRomServices::clearCalls() { calls_.clear(); }

} // namespace saturn::platform
