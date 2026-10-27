#include "saturn/sh2/runtime.hpp"

#include "saturn/sh2/cfg.hpp"
#include "saturn/sir/interpreter.hpp"

#include <algorithm>
#include <limits>
#include <vector>

namespace saturn::sh2 {
namespace {

struct CodeSnapshot {
    core::MemoryMap code;
    std::uint64_t signature{14695981039346656037ULL};
    bool valid{};
};

void mix(std::uint64_t& signature, std::uint8_t byte) noexcept {
    signature ^= byte;
    signature *= 1099511628211ULL;
}

[[nodiscard]] CodeSnapshot snapshotCode(const core::MemoryMap& image,
                                       const sir::MachineState* runtimeMemory,
                                       Address address) {
    CodeSnapshot snapshot;
    std::vector<std::byte> bytes;
    bytes.reserve(64U);
    auto pc = address;
    bool delaySlot = false;
    for (std::size_t offset = 0; offset < 4096U; offset += 2U) {
        if (pc == std::numeric_limits<Address>::max()) break;
        const auto highByte = runtimeMemory
            ? runtimeMemory->readRaw8(pc)
            : image.read8(pc).value_or(0U);
        const auto lowAddress = static_cast<Address>(pc + 1U);
        const auto lowByte = runtimeMemory
            ? runtimeMemory->readRaw8(lowAddress)
            : image.read8(lowAddress).value_or(0U);
        // Sparse runtime memory is zero-filled. A pair of zero bytes is the
        // boundary used by the overlay loader to stop before adjacent data.
        if (highByte == 0U && lowByte == 0U) break;
        bytes.push_back(static_cast<std::byte>(highByte));
        bytes.push_back(static_cast<std::byte>(lowByte));
        if (delaySlot) break;
        const auto instruction = Decoder{}.decode(pc,
            static_cast<std::uint16_t>((highByte << 8U) | lowByte)).instruction;
        if (instruction.flow != FlowKind::Fallthrough) {
            if (instruction.delaySlot == DelaySlot::None) break;
            delaySlot = true;
        }
        if (pc > std::numeric_limits<Address>::max() - 2U) break;
        pc = static_cast<Address>(pc + 2U);
    }
    if (bytes.empty()) return snapshot;

    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        mix(snapshot.signature, static_cast<std::uint8_t>(address >> shift));
    }
    for (const auto byte : bytes) {
        mix(snapshot.signature, std::to_integer<std::uint8_t>(byte));
    }
    snapshot.valid = true;
    snapshot.code = core::MemoryMap(address, std::move(bytes));
    return snapshot;
}

} // namespace

RuntimeBlockResolver::RuntimeBlockResolver(const core::MemoryMap& image, sir::Module& module,
                                         const sir::MachineState* runtimeMemory)
    : image_(image), module_(module), runtimeMemory_(runtimeMemory) {
    blockCache_.reserve(256U);
    blockCodeSignatures_.reserve(256U);
    failedCodeSignatures_.reserve(64U);
}

sir::BasicBlock* RuntimeBlockResolver::resolve(Address address, std::size_t step) {
    ++stats_.requests;
    auto snapshot = snapshotCode(image_, runtimeMemory_, address);
    if (!snapshot.valid && runtimeMemory_) {
        // This fallback is only for addresses present in the immutable boot
        // image when a sparse page was never materialized in MachineState.
        snapshot = snapshotCode(image_, nullptr, address);
    }
    if (!snapshot.valid) {
        ++stats_.failures;
        return nullptr;
    }

    if (const auto failed = failedCodeSignatures_.find(address);
        failed != failedCodeSignatures_.end() && failed->second == snapshot.signature) {
        ++stats_.failures;
        return nullptr;
    }

    auto* existing = module_.blockAt(address);
    if (const auto cached = blockCache_.find(address);
        cached != blockCache_.end() && cached->second == existing) {
        const auto code = blockCodeSignatures_.find(address);
        if (code != blockCodeSignatures_.end() && code->second == snapshot.signature &&
            existing && existing->isLowered()) {
            ++stats_.reusedBlocks;
            return existing;
        }
    }

    if (existing && existing->isLowered()) {
        auto previous = blockCodeSignatures_.find(address);
        if (previous == blockCodeSignatures_.end() && image_.contains(address, 2U)) {
            const auto imageSnapshot = snapshotCode(image_, nullptr, address);
            if (imageSnapshot.valid && imageSnapshot.signature == snapshot.signature) {
                blockCache_.insert_or_assign(address, existing);
                blockCodeSignatures_.insert_or_assign(address, snapshot.signature);
                ++stats_.reusedBlocks;
                return existing;
            }
        } else if (previous != blockCodeSignatures_.end() &&
                   previous->second == snapshot.signature) {
            blockCache_.insert_or_assign(address, existing);
            ++stats_.reusedBlocks;
            return existing;
        }
        std::uint64_t previousSignature{};
        bool previousSignatureKnown{};
        if (previous != blockCodeSignatures_.end()) {
            previousSignature = previous->second;
            previousSignatureKnown = true;
        } else {
            const auto imageSnapshot = snapshotCode(image_, nullptr, address);
            if (imageSnapshot.valid) {
                previousSignature = imageSnapshot.signature;
                previousSignatureKnown = true;
            }
        }
        if (!module_.resetBlockAt(address)) {
            ++stats_.failures;
            return nullptr;
        }
        ++stats_.replacedBlocks;
        replacedAddresses_.push_back(address);
        replacements_.push_back({address, step, previousSignature,
                                 snapshot.signature, previousSignatureKnown});
    }

    const auto cfg = BlockBuilder{}.build(snapshot.code, std::vector<Address>{address});
    const auto report = lowerCFG(cfg, module_);
    stats_.loweredBlocks += report.loweredBlocks;
    stats_.reusedBlocks += report.reusedBlocks;
    auto* resolved = module_.blockAt(address);
    if (!report.success() || !resolved || !resolved->isLowered()) {
        failedCodeSignatures_.insert_or_assign(address, snapshot.signature);
        ++stats_.failures;
        return nullptr;
    }
    failedCodeSignatures_.erase(address);
    blockCache_.insert_or_assign(address, resolved);
    blockCodeSignatures_.insert_or_assign(address, snapshot.signature);
    return resolved;
}

const DynamicResolutionStats& RuntimeBlockResolver::stats() const noexcept {
    return stats_;
}

std::vector<Address> RuntimeBlockResolver::resolvedAddresses() const {
    std::vector<Address> addresses;
    addresses.reserve(blockCache_.size());
    for (const auto& [address, block] : blockCache_) {
        if (block != nullptr) addresses.push_back(address);
    }
    std::sort(addresses.begin(), addresses.end());
    return addresses;
}

const std::vector<Address>& RuntimeBlockResolver::replacedAddresses() const noexcept {
    return replacedAddresses_;
}

const std::vector<RuntimeOverlayReplacement>&
RuntimeBlockResolver::replacements() const noexcept {
    return replacements_;
}

} // namespace saturn::sh2
