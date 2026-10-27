#include "saturn/sir/module.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace saturn::sir {
namespace {

[[nodiscard]] std::string defaultName(const char* prefix, core::Address address) {
    std::ostringstream output;
    output << prefix << '_' << std::uppercase << std::hex << std::setw(8)
           << std::setfill('0') << address;
    return output.str();
}

} // namespace

BasicBlock::BasicBlock(core::Address address, std::string name)
    : address_(address),
      name_(name.empty() ? defaultName("block", address) : std::move(name)) {}

core::Address BasicBlock::address() const noexcept { return address_; }
const std::string& BasicBlock::name() const noexcept { return name_; }
const std::vector<Instruction>& BasicBlock::instructions() const noexcept {
    return instructions_;
}
std::uint64_t BasicBlock::revision() const noexcept { return revision_; }
bool BasicBlock::isLowered() const noexcept { return lowered_; }
void BasicBlock::markLowered() noexcept { lowered_ = true; }

Function::Function(core::Address address, std::string name)
    : address_(address),
      name_(name.empty() ? defaultName("function", address) : std::move(name)) {}

core::Address Function::address() const noexcept { return address_; }
const std::string& Function::name() const noexcept { return name_; }
BasicBlock* Function::entry() const noexcept { return entry_; }
void Function::setEntry(BasicBlock* block) noexcept { entry_ = block; }

BasicBlock& Module::createBlock(core::Address address, std::string name) {
    const auto found = blocks_.find(address);
    if (found != blocks_.end()) {
        return *found->second;
    }
    auto block = std::make_unique<BasicBlock>(address, std::move(name));
    auto* result = block.get();
    blocks_.emplace(address, std::move(block));
    return *result;
}

Function& Module::createFunction(core::Address address, std::string name) {
    const auto found = functions_.find(address);
    if (found != functions_.end()) {
        return *found->second;
    }
    auto function = std::make_unique<Function>(address, std::move(name));
    auto* result = function.get();
    functions_.emplace(address, std::move(function));
    return *result;
}

BasicBlock* Module::blockAt(core::Address address) noexcept {
    const auto found = blocks_.find(address);
    return found == blocks_.end() ? nullptr : found->second.get();
}

const BasicBlock* Module::blockAt(core::Address address) const noexcept {
    const auto found = blocks_.find(address);
    return found == blocks_.end() ? nullptr : found->second.get();
}

BasicBlock* Module::resetBlockAt(core::Address address) noexcept {
    const auto found = blocks_.find(address);
    if (found == blocks_.end()) return nullptr;
    found->second->instructions_.clear();
    ++found->second->revision_;
    found->second->lowered_ = false;
    return found->second.get();
}

Function* Module::functionAt(core::Address address) noexcept {
    const auto found = functions_.find(address);
    return found == functions_.end() ? nullptr : found->second.get();
}

const Function* Module::functionAt(core::Address address) const noexcept {
    const auto found = functions_.find(address);
    return found == functions_.end() ? nullptr : found->second.get();
}

std::size_t Module::blockCount() const noexcept { return blocks_.size(); }
std::size_t Module::functionCount() const noexcept { return functions_.size(); }
std::size_t Module::instructionCount() const noexcept {
    std::size_t count{};
    for (const auto& [_, block] : blocks_) {
        count += block->instructions().size();
    }
    return count;
}

const std::map<core::Address, std::unique_ptr<BasicBlock>>&
Module::blocks() const noexcept {
    return blocks_;
}

Value Module::allocateValue(Type type) noexcept { return {nextValueId_++, type}; }

ModuleBuilder::ModuleBuilder(Module& module) noexcept : module_(module) {}

void ModuleBuilder::setInsertPoint(BasicBlock& block) noexcept { block_ = &block; }
BasicBlock* ModuleBuilder::insertPoint() const noexcept { return block_; }

Instruction& ModuleBuilder::append(Operation operation) {
    if (!block_) {
        throw std::logic_error("SIR builder has no insertion block");
    }
    block_->instructions_.push_back(Instruction{});
    ++block_->revision_;
    auto& instruction = block_->instructions_.back();
    instruction.operation = operation;
    return instruction;
}

Value ModuleBuilder::unary(Operation operation, Value operand, Type resultType) {
    auto& instruction = append(operation);
    instruction.operands.push_back(operand);
    instruction.results.push_back(module_.allocateValue(resultType));
    return instruction.results[0];
}

Value ModuleBuilder::binary(Operation operation, Value left, Value right, Type resultType) {
    auto& instruction = append(operation);
    instruction.operands = {left, right};
    instruction.results.push_back(module_.allocateValue(resultType));
    return instruction.results[0];
}

FlaggedValue ModuleBuilder::flagged(Operation operation, Value left, Value right) {
    auto& instruction = append(operation);
    instruction.operands = {left, right};
    instruction.results = {module_.allocateValue(Type::I32), module_.allocateValue(Type::I1)};
    return {instruction.results[0], instruction.results[1]};
}

FlaggedValue ModuleBuilder::flagged(Operation operation, Value left, Value right,
                                    Value flagIn) {
    auto& instruction = append(operation);
    instruction.operands = {left, right, flagIn};
    instruction.results = {module_.allocateValue(Type::I32), module_.allocateValue(Type::I1)};
    return {instruction.results[0], instruction.results[1]};
}

Value ModuleBuilder::readState(StateSlot slot, Type type) {
    auto& instruction = append(Operation::ReadState);
    instruction.stateSlot = slot;
    instruction.results.push_back(module_.allocateValue(type));
    return instruction.results[0];
}

void ModuleBuilder::writeState(StateSlot slot, Value value) {
    auto& instruction = append(Operation::WriteState);
    instruction.stateSlot = slot;
    instruction.operands.push_back(value);
}

Value ModuleBuilder::constI1(bool value) {
    auto& instruction = append(Operation::ConstI1);
    instruction.immediate = value ? 1 : 0;
    instruction.results.push_back(module_.allocateValue(Type::I1));
    return instruction.results[0];
}

Value ModuleBuilder::constI32(std::int32_t value) {
    auto& instruction = append(Operation::ConstI32);
    instruction.immediate = value;
    instruction.results.push_back(module_.allocateValue(Type::I32));
    return instruction.results[0];
}

Value ModuleBuilder::copyI32(Value value) { return unary(Operation::CopyI32, value, Type::I32); }
Value ModuleBuilder::addI32(Value left, Value right) { return binary(Operation::AddI32, left, right, Type::I32); }
Value ModuleBuilder::subI32(Value left, Value right) { return binary(Operation::SubI32, left, right, Type::I32); }
Value ModuleBuilder::mulI32(Value left, Value right) { return binary(Operation::MulI32, left, right, Type::I32); }
FlaggedValue ModuleBuilder::addWithCarryI32(Value left, Value right, Value carryIn) { return flagged(Operation::AddWithCarryI32, left, right, carryIn); }
FlaggedValue ModuleBuilder::subWithBorrowI32(Value left, Value right, Value borrowIn) { return flagged(Operation::SubWithBorrowI32, left, right, borrowIn); }
FlaggedValue ModuleBuilder::addWithOverflowI32(Value left, Value right) { return flagged(Operation::AddWithOverflowI32, left, right); }
FlaggedValue ModuleBuilder::subWithOverflowI32(Value left, Value right) { return flagged(Operation::SubWithOverflowI32, left, right); }
Value ModuleBuilder::andI32(Value left, Value right) { return binary(Operation::AndI32, left, right, Type::I32); }
Value ModuleBuilder::orI32(Value left, Value right) { return binary(Operation::OrI32, left, right, Type::I32); }
Value ModuleBuilder::xorI32(Value left, Value right) { return binary(Operation::XorI32, left, right, Type::I32); }
Value ModuleBuilder::xorI1(Value left, Value right) { return binary(Operation::XorI1, left, right, Type::I1); }
Value ModuleBuilder::notI32(Value value) { return unary(Operation::NotI32, value, Type::I32); }
Value ModuleBuilder::shiftLeftI32(Value value, Value amount) { return binary(Operation::ShiftLeftI32, value, amount, Type::I32); }
Value ModuleBuilder::logicalShiftRightI32(Value value, Value amount) { return binary(Operation::LogicalShiftRightI32, value, amount, Type::I32); }
Value ModuleBuilder::arithmeticShiftRightI32(Value value, Value amount) { return binary(Operation::ArithmeticShiftRightI32, value, amount, Type::I32); }

Value ModuleBuilder::extractBitI32(Value value, std::uint8_t bit) {
    auto& instruction = append(Operation::ExtractBitI32);
    instruction.operands.push_back(value);
    instruction.immediate = bit;
    instruction.results.push_back(module_.allocateValue(Type::I1));
    return instruction.results[0];
}
Value ModuleBuilder::truncateI32ToI8(Value value) { return unary(Operation::TruncateI32ToI8, value, Type::I8); }
Value ModuleBuilder::truncateI32ToI16(Value value) { return unary(Operation::TruncateI32ToI16, value, Type::I16); }
Value ModuleBuilder::signExtendI8ToI32(Value value) { return unary(Operation::SignExtendI8ToI32, value, Type::I32); }
Value ModuleBuilder::signExtendI16ToI32(Value value) { return unary(Operation::SignExtendI16ToI32, value, Type::I32); }
Value ModuleBuilder::zeroExtendI1ToI32(Value value) { return unary(Operation::ZeroExtendI1ToI32, value, Type::I32); }
Value ModuleBuilder::cmpEqI32(Value left, Value right) { return binary(Operation::CmpEqI32, left, right, Type::I1); }
Value ModuleBuilder::cmpSignedGreaterEqualI32(Value left, Value right) { return binary(Operation::CmpSignedGreaterEqualI32, left, right, Type::I1); }
Value ModuleBuilder::cmpUnsignedGreaterEqualI32(Value left, Value right) { return binary(Operation::CmpUnsignedGreaterEqualI32, left, right, Type::I1); }
Value ModuleBuilder::cmpSignedGreaterI32(Value left, Value right) { return binary(Operation::CmpSignedGreaterI32, left, right, Type::I1); }
Value ModuleBuilder::cmpUnsignedGreaterI32(Value left, Value right) { return binary(Operation::CmpUnsignedGreaterI32, left, right, Type::I1); }

DivideStepResult ModuleBuilder::divideStepI32(Value dividend, Value divisor, Value q, Value m, Value t) {
    auto& instruction = append(Operation::DivideStepI32);
    instruction.operands = {dividend, divisor, q, m, t};
    instruction.results = {module_.allocateValue(Type::I32), module_.allocateValue(Type::I1), module_.allocateValue(Type::I1)};
    return {instruction.results[0], instruction.results[1], instruction.results[2]};
}
Value ModuleBuilder::load8(Value address) { return unary(Operation::Load8, address, Type::I8); }
Value ModuleBuilder::load16(Value address) { return unary(Operation::Load16, address, Type::I16); }
Value ModuleBuilder::load32(Value address) { return unary(Operation::Load32, address, Type::I32); }
void ModuleBuilder::store8(Value address, Value value) { append(Operation::Store8).operands = {address, value}; }
void ModuleBuilder::store16(Value address, Value value) { append(Operation::Store16).operands = {address, value}; }
void ModuleBuilder::store32(Value address, Value value) { append(Operation::Store32).operands = {address, value}; }
void ModuleBuilder::branch(BasicBlock* target) { append(Operation::Branch).target = target; }

void ModuleBuilder::branchIf(Value condition, BasicBlock* taken, BasicBlock* notTaken) {
    auto& instruction = append(Operation::BranchIf);
    instruction.operands.push_back(condition);
    instruction.target = taken;
    instruction.alternative = notTaken;
}
void ModuleBuilder::branchIndirect(Value target) { append(Operation::BranchIndirect).operands.push_back(target); }
void ModuleBuilder::call(Function* target, BasicBlock* returnSite) {
    auto& instruction = append(Operation::Call);
    instruction.function = target;
    instruction.returnSite = returnSite;
}
void ModuleBuilder::callIndirect(Value target, BasicBlock* returnSite) {
    auto& instruction = append(Operation::CallIndirect);
    instruction.operands.push_back(target);
    instruction.returnSite = returnSite;
}
void ModuleBuilder::ret(Value target) { append(Operation::Return).operands.push_back(target); }
void ModuleBuilder::returnFromException() { append(Operation::ReturnFromException); }
void ModuleBuilder::trap(std::uint8_t vector) { append(Operation::Trap).immediate = vector; }
void ModuleBuilder::sleep() { append(Operation::Sleep); }

std::string_view version() noexcept { return "SIR-L0 (concrete module and interpreter M1)"; }

} // namespace saturn::sir
