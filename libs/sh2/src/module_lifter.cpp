#include "saturn/sh2/module_lifter.hpp"

#include <array>
#include <optional>

namespace saturn::sh2 {
namespace {

constexpr std::size_t kRegisterCount = static_cast<std::size_t>(Register::SR) + 1U;

class ConcreteLiftContext final : public BlockLiftContext {
public:
    ConcreteLiftContext(sir::Module& module, sir::ModuleBuilder& builder)
        : module_(module), builder_(builder) {}

    void begin(sir::BasicBlock& block) {
        builder_.setInsertPoint(block);
        registers_.fill(std::nullopt);
        t_.reset();
        m_.reset();
        q_.reset();
        sourcePC_ = block.address();
    }

    sir::Value read(Register reg) override {
        auto& value = registers_[static_cast<std::size_t>(reg)];
        if (!value) {
            value = builder_.readState(stateSlot(reg));
        }
        return *value;
    }

    void write(Register reg, sir::Value value) override {
        registers_[static_cast<std::size_t>(reg)] = value;
        builder_.writeState(stateSlot(reg), value);
    }

    sir::Value readT() override { return readFlag(t_, kStateSlotT); }
    void writeT(sir::Value value) override { writeFlag(t_, kStateSlotT, value); }
    sir::Value readM() override { return readFlag(m_, kStateSlotM); }
    void writeM(sir::Value value) override { writeFlag(m_, kStateSlotM, value); }
    sir::Value readQ() override { return readFlag(q_, kStateSlotQ); }
    void writeQ(sir::Value value) override { writeFlag(q_, kStateSlotQ, value); }

    Address sourcePC() const override { return sourcePC_; }
    sir::Builder& builder() override { return builder_; }

    sir::BasicBlock* blockFor(Address address) override {
        return module_.blockAt(address);
    }
    sir::Function* functionFor(Address address) override {
        return module_.functionAt(address);
    }

private:
    sir::Value readFlag(std::optional<sir::Value>& current, sir::StateSlot slot) {
        if (!current) {
            current = builder_.readState(slot, sir::Type::I1);
        }
        return *current;
    }

    void writeFlag(std::optional<sir::Value>& current, sir::StateSlot slot,
                   sir::Value value) {
        current = value;
        builder_.writeState(slot, value);
    }

    sir::Module& module_;
    sir::ModuleBuilder& builder_;
    std::array<std::optional<sir::Value>, kRegisterCount> registers_{};
    std::optional<sir::Value> t_;
    std::optional<sir::Value> m_;
    std::optional<sir::Value> q_;
    Address sourcePC_{};
};

} // namespace

ModuleLiftReport lowerCFG(const CFG& cfg, sir::Module& module) {
    for (const auto& [address, block] : cfg.blocks) {
        module.createBlock(address);
        for (const auto& edge : block.successors) {
            if (edge.target) {
                module.createBlock(*edge.target);
            }
        }
    }

    for (const auto& candidate : cfg.functionCandidates) {
        auto& function = module.createFunction(candidate.address);
        function.setEntry(module.blockAt(candidate.address));
    }
    for (const auto& [_, block] : cfg.blocks) {
        if (!block.terminator) {
            continue;
        }
        const auto& transfer = block.terminator->transfer;
        if ((transfer.flow == FlowKind::DirectCall ||
             transfer.flow == FlowKind::IndirectCall) &&
            transfer.directTarget) {
            auto& function = module.createFunction(*transfer.directTarget);
            function.setEntry(module.blockAt(*transfer.directTarget));
        }
    }

    sir::ModuleBuilder builder(module);
    ConcreteLiftContext context(module, builder);
    ModuleLiftReport report;
    for (const auto& [address, block] : cfg.blocks) {
        auto* output = module.blockAt(address);
        if (!output) {
            report.failures.push_back({BlockLiftStatus::MissingTarget, address});
            continue;
        }
        if (output->isLowered()) {
            ++report.reusedBlocks;
            continue;
        }
        context.begin(*output);
        const auto result = lowerBlock(block, context);
        if (result) {
            output->markLowered();
            ++report.loweredBlocks;
        } else {
            report.failures.push_back(result);
        }
    }
    return report;
}

} // namespace saturn::sh2
