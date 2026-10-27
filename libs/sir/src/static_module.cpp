#include "saturn/sir/static_module.hpp"

#include <algorithm>
#include <bit>
#include <iomanip>
#include <map>
#include <ostream>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace saturn::sir {
namespace {

void mix(StaticBlockSignature& signature, std::uint64_t value) noexcept {
    signature.first ^= value;
    signature.first *= 1099511628211ULL;
    signature.second ^= value + 0x9E3779B97F4A7C15ULL +
                        (signature.second << 6U) + (signature.second >> 2U);
    signature.second *= 0xD6E8FEB86659FD93ULL;
    signature.second ^= signature.second >> 29U;
}

[[nodiscard]] std::string addressLiteral(core::Address address) {
    std::ostringstream text;
    text << "0x" << std::uppercase << std::hex << std::setw(8)
         << std::setfill('0') << address << "U";
    return text.str();
}

[[nodiscard]] std::string functionName(std::size_t moduleIndex,
                                       core::Address address) {
    std::ostringstream text;
    text << "execute_m" << std::dec << moduleIndex << '_' << std::uppercase
         << std::hex << std::setw(8)
         << std::setfill('0') << address;
    return text.str();
}

[[nodiscard]] std::string valueName(Value value) {
    return "v_" + std::to_string(value.id);
}

[[nodiscard]] std::string normalized(std::string expression, Type type) {
    expression = "(" + expression + ")";
    switch (type) {
    case Type::I1: expression = "(" + expression + " & 1U)"; break;
    case Type::I8: expression = "(" + expression + " & 0xFFU)"; break;
    case Type::I16: expression = "(" + expression + " & 0xFFFFU)"; break;
    case Type::Void:
    case Type::I32:
    case Type::I64:
    case Type::Address: break;
    }
    return "static_cast<std::uint32_t>(" + expression + ")";
}

void assign(std::ostream& output, Value result, std::string expression) {
    output << "    " << valueName(result) << " = "
           << normalized(std::move(expression), result.type) << ";\n";
}

[[nodiscard]] core::Address blockAddress(const BasicBlock* block) noexcept {
    return block ? block->address() : 0U;
}

[[nodiscard]] core::Address functionAddress(const Function* function) noexcept {
    return function ? function->address() : 0U;
}

[[nodiscard]] bool emitInstruction(std::ostream& output,
                                   const Instruction& instruction,
                                   std::string& error) {
    const auto result = [&](std::size_t index = 0U) {
        return instruction.results.at(index);
    };
    const auto operand = [&](std::size_t index) {
        return valueName(instruction.operands.at(index));
    };
    const auto emitTerminal = [&](std::string_view control,
                                  std::string_view field,
                                  std::string_view value) {
        output << "    result.control = ControlKind::" << control << ";\n"
               << "    result." << field << " = " << value << ";\n"
               << "    return result;\n";
    };

    switch (instruction.operation) {
    case Operation::ConstI1:
    case Operation::ConstI32:
        if (instruction.results.size() != 1U) break;
        assign(output, result(), std::to_string(instruction.immediate) + "LL");
        return false;
    case Operation::CopyI32:
    case Operation::NotI32:
    case Operation::TruncateI32ToI8:
    case Operation::TruncateI32ToI16:
    case Operation::SignExtendI8ToI32:
    case Operation::SignExtendI16ToI32:
    case Operation::ZeroExtendI1ToI32:
        if (instruction.results.size() != 1U || instruction.operands.size() != 1U) break;
        switch (instruction.operation) {
        case Operation::CopyI32:
            assign(output, result(), operand(0));
            break;
        case Operation::NotI32:
            assign(output, result(), "~" + operand(0));
            break;
        case Operation::TruncateI32ToI8:
            assign(output, result(), operand(0) + " & 0xFFU");
            break;
        case Operation::TruncateI32ToI16:
            assign(output, result(), operand(0) + " & 0xFFFFU");
            break;
        case Operation::SignExtendI8ToI32:
            assign(output, result(), "((" + operand(0) + " & 0x80U) != 0U ? " +
                                      "(" + operand(0) + " | 0xFFFFFF00U) : (" +
                                      operand(0) + " & 0xFFU))");
            break;
        case Operation::SignExtendI16ToI32:
            assign(output, result(), "((" + operand(0) + " & 0x8000U) != 0U ? " +
                                      "(" + operand(0) + " | 0xFFFF0000U) : (" +
                                      operand(0) + " & 0xFFFFU))");
            break;
        case Operation::ZeroExtendI1ToI32:
            assign(output, result(), operand(0) + " & 1U");
            break;
        default: break;
        }
        return false;
    case Operation::AddI32:
    case Operation::SubI32:
    case Operation::MulI32:
    case Operation::AndI32:
    case Operation::OrI32:
    case Operation::XorI32:
    case Operation::XorI1:
    case Operation::ShiftLeftI32:
    case Operation::LogicalShiftRightI32:
    case Operation::ArithmeticShiftRightI32:
    case Operation::CmpEqI32:
    case Operation::CmpSignedGreaterEqualI32:
    case Operation::CmpUnsignedGreaterEqualI32:
    case Operation::CmpSignedGreaterI32:
    case Operation::CmpUnsignedGreaterI32: {
        if (instruction.results.size() != 1U || instruction.operands.size() != 2U) break;
        const auto left = operand(0);
        const auto right = operand(1);
        std::string expression;
        switch (instruction.operation) {
        case Operation::AddI32: expression = left + " + " + right; break;
        case Operation::SubI32: expression = left + " - " + right; break;
        case Operation::MulI32: expression = left + " * " + right; break;
        case Operation::AndI32: expression = left + " & " + right; break;
        case Operation::OrI32: expression = left + " | " + right; break;
        case Operation::XorI32:
        case Operation::XorI1: expression = left + " ^ " + right; break;
        case Operation::ShiftLeftI32: expression = left + " << (" + right + " & 31U)"; break;
        case Operation::LogicalShiftRightI32: expression = left + " >> (" + right + " & 31U)"; break;
        case Operation::ArithmeticShiftRightI32:
            expression = "static_cast<std::uint32_t>(std::bit_cast<std::int32_t>(" +
                         left + ") >> (" + right + " & 31U))";
            break;
        case Operation::CmpEqI32: expression = left + " == " + right; break;
        case Operation::CmpSignedGreaterEqualI32:
            expression = "std::bit_cast<std::int32_t>(" + left + ") >= " +
                         "std::bit_cast<std::int32_t>(" + right + ")";
            break;
        case Operation::CmpUnsignedGreaterEqualI32: expression = left + " >= " + right; break;
        case Operation::CmpSignedGreaterI32:
            expression = "std::bit_cast<std::int32_t>(" + left + ") > " +
                         "std::bit_cast<std::int32_t>(" + right + ")";
            break;
        case Operation::CmpUnsignedGreaterI32: expression = left + " > " + right; break;
        default: break;
        }
        assign(output, result(), std::move(expression));
        return false;
    }
    case Operation::AddWithCarryI32:
    case Operation::SubWithBorrowI32: {
        if (instruction.results.size() != 2U || instruction.operands.size() != 3U) break;
        const auto left = operand(0);
        const auto right = operand(1);
        const auto flag = operand(2);
        if (instruction.operation == Operation::AddWithCarryI32) {
            output << "    const std::uint64_t wide_" << result().id << " = "
                   << "static_cast<std::uint64_t>(" << left << ") + " << right
                   << " + (" << flag << " & 1U);\n";
            assign(output, result(0), "wide_" + std::to_string(result().id));
            assign(output, result(1), "wide_" + std::to_string(result().id) + " >> 32U");
        } else {
            output << "    const std::uint64_t subtrahend_" << result().id << " = "
                   << "static_cast<std::uint64_t>(" << right << ") + (" << flag << " & 1U);\n";
            assign(output, result(0), left + " - static_cast<std::uint32_t>(subtrahend_" +
                                      std::to_string(result().id) + ")");
            assign(output, result(1), "static_cast<std::uint64_t>(" + left + ") < subtrahend_" +
                                      std::to_string(result().id));
        }
        return false;
    }
    case Operation::AddWithOverflowI32:
    case Operation::SubWithOverflowI32: {
        if (instruction.results.size() != 2U || instruction.operands.size() != 2U) break;
        const auto left = operand(0);
        const auto right = operand(1);
        const auto temporary = "overflow_value_" + std::to_string(result().id);
        const auto add = instruction.operation == Operation::AddWithOverflowI32;
        output << "    const std::uint32_t " << temporary << " = " << left
               << (add ? " + " : " - ") << right << ";\n";
        assign(output, result(0), temporary);
        assign(output, result(1), add
            ? "(~(" + left + " ^ " + right + ") & (" + left + " ^ " + temporary + ")) >> 31U"
            : "((" + left + " ^ " + right + ") & (" + left + " ^ " + temporary + ")) >> 31U");
        return false;
    }
    case Operation::ExtractBitI32:
        if (instruction.results.size() != 1U || instruction.operands.size() != 1U ||
            instruction.immediate < 0 || instruction.immediate > 31) break;
        assign(output, result(), operand(0) + " >> " + std::to_string(instruction.immediate) + "U");
        return false;
    case Operation::DivideStepI32: {
        if (instruction.results.size() != 3U || instruction.operands.size() != 5U) break;
        const auto suffix = std::to_string(result().id);
        output << "    std::uint32_t divide_value_" << suffix << "{};\n"
               << "    std::uint32_t divide_q_" << suffix << "{};\n"
               << "    std::uint32_t divide_t_" << suffix << "{};\n";
        output << "    staticDivideStep(" << operand(0) << ", " << operand(1) << ", "
               << operand(2) << " != 0U, " << operand(3) << " != 0U, "
               << operand(4) << " != 0U, divide_value_" << suffix
               << ", divide_q_" << suffix << ", divide_t_" << suffix << ");\n";
        assign(output, result(0), "divide_value_" + suffix);
        assign(output, result(1), "divide_q_" + suffix);
        assign(output, result(2), "divide_t_" + suffix);
        return false;
    }
    case Operation::Load8:
    case Operation::Load16:
    case Operation::Load32: {
        if (instruction.results.size() != 1U || instruction.operands.size() != 1U) break;
        const auto method = instruction.operation == Operation::Load8 ? "read8"
            : instruction.operation == Operation::Load16 ? "read16BE" : "read32BE";
        assign(output, result(), "state." + std::string(method) +
                                  "(static_cast<::saturn::core::Address>(" + operand(0) + "))");
        return false;
    }
    case Operation::Store8:
    case Operation::Store16:
    case Operation::Store32: {
        if (!instruction.results.empty() || instruction.operands.size() != 2U) break;
        const auto method = instruction.operation == Operation::Store8 ? "write8"
            : instruction.operation == Operation::Store16 ? "write16BE" : "write32BE";
        const auto cast = instruction.operation == Operation::Store8 ? "std::uint8_t"
            : instruction.operation == Operation::Store16 ? "std::uint16_t" : "std::uint32_t";
        output << "    state." << method << "(static_cast<::saturn::core::Address>("
               << operand(0) << "), static_cast<" << cast << ">(" << operand(1) << "));\n";
        return false;
    }
    case Operation::ReadState:
        if (instruction.results.size() != 1U || !instruction.operands.empty()) break;
        assign(output, result(), "state.readState(" + std::to_string(instruction.stateSlot) + "U)");
        return false;
    case Operation::WriteState:
        if (!instruction.results.empty() || instruction.operands.size() != 1U) break;
        output << "    state.writeState(" << instruction.stateSlot << "U, "
               << operand(0) << ");\n";
        return false;
    case Operation::Branch:
        if (!instruction.operands.empty() || !instruction.results.empty()) break;
        emitTerminal("Branch", "target", "module.blockAt(" +
                     addressLiteral(blockAddress(instruction.target)) + ")");
        return true;
    case Operation::BranchIf:
        if (instruction.operands.size() != 1U || !instruction.results.empty()) break;
        output << "    result.control = ControlKind::Branch;\n"
               << "    result.target = " << operand(0) << " != 0U ? module.blockAt("
               << addressLiteral(blockAddress(instruction.target)) << ") : module.blockAt("
               << addressLiteral(blockAddress(instruction.alternative)) << ");\n"
               << "    return result;\n";
        return true;
    case Operation::BranchIndirect:
        if (instruction.operands.size() != 1U || !instruction.results.empty()) break;
        emitTerminal("IndirectBranch", "dynamicTarget", operand(0));
        return true;
    case Operation::Call:
        if (!instruction.operands.empty() || !instruction.results.empty()) break;
        output << "    result.control = ControlKind::Call;\n"
               << "    result.function = module.functionAt("
               << addressLiteral(functionAddress(instruction.function)) << ");\n"
               << "    result.returnSite = module.blockAt("
               << addressLiteral(blockAddress(instruction.returnSite)) << ");\n"
               << "    return result;\n";
        return true;
    case Operation::CallIndirect:
        if (instruction.operands.size() != 1U || !instruction.results.empty()) break;
        output << "    result.control = ControlKind::IndirectCall;\n"
               << "    result.dynamicTarget = " << operand(0) << ";\n"
               << "    result.returnSite = module.blockAt("
               << addressLiteral(blockAddress(instruction.returnSite)) << ");\n"
               << "    return result;\n";
        return true;
    case Operation::Return:
        if (instruction.operands.size() != 1U || !instruction.results.empty()) break;
        emitTerminal("Return", "dynamicTarget", operand(0));
        return true;
    case Operation::ReturnFromException:
        if (!instruction.operands.empty() || !instruction.results.empty()) break;
        output << "    result.control = ControlKind::ExceptionReturn;\n"
               << "    return result;\n";
        return true;
    case Operation::Trap:
        if (!instruction.operands.empty() || !instruction.results.empty()) break;
        output << "    result.control = ControlKind::Trap;\n"
               << "    result.trapVector = static_cast<std::uint8_t>("
               << instruction.immediate << "U);\n"
               << "    return result;\n";
        return true;
    case Operation::Sleep:
        if (!instruction.operands.empty() || !instruction.results.empty()) break;
        output << "    result.slept = true;\n    return result;\n";
        return true;
    }
    error = "malformed or unsupported SIR instruction in static module";
    return false;
}

void emitDivideHelper(std::ostream& output) {
    output << R"CPP(
void staticDivideStep(std::uint32_t dividend, std::uint32_t divisor,
                      bool oldQ, bool m, bool t,
                      std::uint32_t& result, std::uint32_t& outQ,
                      std::uint32_t& outT) noexcept {
    bool q = (dividend & 0x80000000U) != 0;
    result = static_cast<std::uint32_t>((dividend << 1U) | (t ? 1U : 0U));
    std::uint32_t previous{};
    bool carryOrBorrow{};
    if (!oldQ) {
        if (!m) {
            previous = result;
            result = static_cast<std::uint32_t>(result - divisor);
            carryOrBorrow = result > previous;
            q = q ? !carryOrBorrow : carryOrBorrow;
        } else {
            previous = result;
            result = static_cast<std::uint32_t>(result + divisor);
            carryOrBorrow = result < previous;
            q = q ? carryOrBorrow : !carryOrBorrow;
        }
    } else if (!m) {
        previous = result;
        result = static_cast<std::uint32_t>(result + divisor);
        carryOrBorrow = result < previous;
        q = q ? !carryOrBorrow : carryOrBorrow;
    } else {
        previous = result;
        result = static_cast<std::uint32_t>(result - divisor);
        carryOrBorrow = result > previous;
        q = q ? carryOrBorrow : !carryOrBorrow;
    }
    outQ = q ? 1U : 0U;
    outT = q == m ? 1U : 0U;
}
)CPP";
}

} // namespace

StaticBlockSignature staticBlockSignature(const BasicBlock& block) {
    StaticBlockSignature signature{14695981039346656037ULL,
                                   0xA0761D6478BD642FULL};
    mix(signature, block.address());
    std::unordered_map<std::uint32_t, std::uint32_t> canonicalValues;
    std::uint32_t nextCanonical{};
    const auto canonical = [&](Value value) {
        const auto found = canonicalValues.find(value.id);
        return found == canonicalValues.end() ? 0xFFFFFFFFU : found->second;
    };
    for (const auto& instruction : block.instructions()) {
        mix(signature, static_cast<std::uint8_t>(instruction.operation));
        mix(signature, static_cast<std::uint64_t>(instruction.immediate));
        mix(signature, instruction.stateSlot);
        mix(signature, instruction.results.size());
        for (const auto result : instruction.results) {
            mix(signature, static_cast<std::uint8_t>(result.type));
            canonicalValues.insert_or_assign(result.id, nextCanonical++);
        }
        mix(signature, instruction.operands.size());
        for (const auto value : instruction.operands) {
            mix(signature, canonical(value));
            mix(signature, static_cast<std::uint8_t>(value.type));
        }
        mix(signature, blockAddress(instruction.target));
        mix(signature, blockAddress(instruction.alternative));
        mix(signature, blockAddress(instruction.returnSite));
        mix(signature, functionAddress(instruction.function));
    }
    return signature;
}

StaticNativeBlockExecutor::StaticNativeBlockExecutor(
    const Module& module, std::span<const StaticNativeBlockEntry> blocks) noexcept
    : module_(module), blocks_(blocks) {}

std::optional<ExecutionResult> StaticNativeBlockExecutor::execute(
    const BasicBlock& block, MachineState& state) {
    const auto cached = resolved_.find(&block);
    if (cached != resolved_.end()) {
        if (cached->second.revision == block.revision()) {
            if (!cached->second.function) return std::nullopt;
            ++executedBlockCount_;
            return cached->second.function(module_, state);
        }
    }
    const auto signature = staticBlockSignature(block);
    const bool changedVersion = cached != resolved_.end() &&
                                cached->second.signature != signature;
    const auto found = std::lower_bound(blocks_.begin(), blocks_.end(), block.address(),
        [](const StaticNativeBlockEntry& entry, core::Address address) {
            return entry.address < address;
        });
    auto matching = found;
    while (matching != blocks_.end() && matching->address == block.address() &&
           matching->signature != signature) {
        ++matching;
    }
    if (matching == blocks_.end() || matching->address != block.address() ||
        !matching->function) {
        resolved_.insert_or_assign(&block,
            CachedResolution{block.revision(), signature, nullptr});
        return std::nullopt;
    }
    if (changedVersion) overlayVariantSelections_.push_back(block.address());
    resolved_.insert_or_assign(&block, CachedResolution{
        block.revision(), signature, matching->function});
    ++executedBlockCount_;
    return matching->function(module_, state);
}

std::size_t StaticNativeBlockExecutor::blockCount() const noexcept {
    return blocks_.size();
}

std::size_t StaticNativeBlockExecutor::executedBlockCount() const noexcept {
    return executedBlockCount_;
}

const std::vector<core::Address>&
StaticNativeBlockExecutor::overlayVariantSelections() const noexcept {
    return overlayVariantSelections_;
}

StaticModuleEmitReport emitStaticModulesCpp(
    std::span<const Module* const> modules, std::ostream& output,
    const StaticModuleEmitOptions& options) {
    StaticModuleEmitReport report;
    struct EmittedBlock {
        std::size_t moduleIndex{};
        const BasicBlock* block{};
        StaticBlockSignature signature{};
    };
    std::vector<EmittedBlock> lowered;
    for (std::size_t moduleIndex = 0; moduleIndex < modules.size(); ++moduleIndex) {
        if (modules[moduleIndex] == nullptr) {
            report.error = "null module in static module set";
            return report;
        }
        lowered.reserve(lowered.size() + modules[moduleIndex]->blockCount());
        for (const auto& [address, block] : modules[moduleIndex]->blocks()) {
            (void)address;
            if (options.addressBank &&
                static_cast<std::uint16_t>(block->address() >> 16U) !=
                    *options.addressBank) {
                continue;
            }
            if (block->isLowered()) {
                lowered.push_back({moduleIndex, block.get(),
                                   staticBlockSignature(*block)});
            } else {
                report.skippedBlocks.push_back(block->address());
            }
        }
    }
    std::sort(lowered.begin(), lowered.end(), [](const EmittedBlock& left,
                                                 const EmittedBlock& right) {
        if (left.block->address() != right.block->address()) {
            return left.block->address() < right.block->address();
        }
        if (left.signature.first != right.signature.first) {
            return left.signature.first < right.signature.first;
        }
        if (left.signature.second != right.signature.second) {
            return left.signature.second < right.signature.second;
        }
        return left.moduleIndex < right.moduleIndex;
    });
    lowered.erase(std::unique(lowered.begin(), lowered.end(),
        [](const EmittedBlock& left, const EmittedBlock& right) {
            return left.block->address() == right.block->address() &&
                   left.signature == right.signature;
        }), lowered.end());

    output << "// Generated by saturnbox-aot. Compile and link with saturn-sir.\n"
           << "#include \"saturn/sir/static_module.hpp\"\n"
           << "#include <array>\n#include <bit>\n#include <cstdint>\n\n"
           << "namespace {\nusing namespace ::saturn::sir;\n";
    emitDivideHelper(output);
    for (const auto& emitted : lowered) {
        const auto* block = emitted.block;
        output << "\nExecutionResult "
               << functionName(emitted.moduleIndex, block->address())
               << "(const Module& module, MachineState& state) {\n"
               << "    (void)module;\n"
               << "    (void)state;\n"
               << "    ExecutionResult result{};\n";
        std::map<std::uint32_t, Type> values;
        for (const auto& instruction : block->instructions()) {
            for (const auto value : instruction.results) {
                values.insert_or_assign(value.id, value.type);
            }
        }
        for (const auto& [id, _] : values) {
            output << "    std::uint32_t v_" << id << "{};\n";
        }
        for (const auto& instruction : block->instructions()) {
            std::string error;
            if (emitInstruction(output, instruction, error)) {
                break;
            }
            if (!error.empty()) {
                report.error = "block " + addressLiteral(block->address()) + ": " + error;
                return report;
            }
        }
        if (block->instructions().empty() ||
            (block->instructions().back().operation != Operation::Branch &&
             block->instructions().back().operation != Operation::BranchIf &&
             block->instructions().back().operation != Operation::BranchIndirect &&
             block->instructions().back().operation != Operation::Call &&
             block->instructions().back().operation != Operation::CallIndirect &&
             block->instructions().back().operation != Operation::Return &&
             block->instructions().back().operation != Operation::ReturnFromException &&
             block->instructions().back().operation != Operation::Trap &&
             block->instructions().back().operation != Operation::Sleep)) {
            output << "    result.status = InterpreterStatus::MissingTerminator;\n"
                   << "    return result;\n";
        }
        output << "}\n";
    }

    output << "\nconstexpr std::array<StaticNativeBlockEntry, " << lowered.size()
           << "> kStaticBlocks{{\n";
    for (const auto& emitted : lowered) {
        const auto* block = emitted.block;
        const auto signature = emitted.signature;
        output << "    {" << addressLiteral(block->address()) << ", {0x"
               << std::uppercase << std::hex << signature.first << "ULL, 0x"
               << signature.second << "ULL}, &"
               << functionName(emitted.moduleIndex, block->address())
               << "},\n" << std::dec;
    }
    output << "}};\n} // namespace\n\n"
           << "extern \"C\" const ::saturn::sir::StaticNativeBlockEntry* "
           << options.registryFunction << "(std::size_t* count) noexcept {\n"
           << "    if (count) *count = kStaticBlocks.size();\n"
           << "    return kStaticBlocks.data();\n}\n";
    report.blocksEmitted = lowered.size();
    for (const auto& emitted : lowered) {
        report.instructionsEmitted += emitted.block->instructions().size();
    }
    if (!output) report.error = "unable to write generated C++ source";
    return report;
}

StaticModuleEmitReport emitStaticModulesCpp(
    std::span<const Module* const> modules, std::ostream& output) {
    return emitStaticModulesCpp(modules, output, StaticModuleEmitOptions{});
}

StaticModuleEmitReport emitStaticModuleCpp(const Module& module,
                                           std::ostream& output) {
    const Module* modules[]{&module};
    return emitStaticModulesCpp(modules, output);
}

} // namespace saturn::sir
