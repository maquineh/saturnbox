#include "test_support.hpp"
#include "saturn/saturn/m68k.hpp"

#include <array>
#include <stdexcept>

namespace {
struct BusFixture {
    std::array<std::uint8_t, 0x10000> ram{};
    std::array<unsigned, 3> writes{};

    std::uint32_t read(std::uint32_t address, unsigned width) const {
        std::uint32_t value{};
        for (unsigned index = 0; index < width; ++index)
            value = (value << 8U) | ram[(address + index) & 0xFFFFU];
        return value;
    }
    void write(std::uint32_t address, unsigned width, std::uint32_t value) {
        for (unsigned index = 0; index < width; ++index)
            ram[(address + index) & 0xFFFFU] = static_cast<std::uint8_t>(
                value >> ((width - index - 1U) * 8U));
    }
    saturn::platform::M68k::Bus bus() {
        return {
            [this](auto a) { return static_cast<std::uint8_t>(read(a, 1)); },
            [this](auto a) { return static_cast<std::uint16_t>(read(a, 2)); },
            [this](auto a) { return read(a, 4); },
            [this](auto a, std::uint8_t v) { ++writes[0]; write(a, 1, v); },
            [this](auto a, std::uint16_t v) { ++writes[1]; write(a, 2, v); },
            [this](auto a, std::uint32_t v) { ++writes[2]; write(a, 4, v); }};
    }
};

void executesReal68000InstructionsAndInterrupt() {
    BusFixture fixture;
    fixture.write(0, 4, 0x8000U);
    fixture.write(4, 4, 0x100U);
    fixture.write(26U * 4U, 4, 0x180U); // Autovector level 2.
    const std::uint16_t program[] = {
        0x13FC, 0x0012, 0x0000, 0x0200, // MOVE.B #$12,$200
        0x33FC, 0x3456, 0x0000, 0x0202, // MOVE.W #$3456,$202
        0x23FC, 0x789A, 0xBCDE, 0x0000, 0x0204, // MOVE.L
        0x4E72, 0x2000, // STOP with interrupts enabled.
        0x60FE}; // BRA to itself after interrupt return.
    for (unsigned index = 0; index < std::size(program); ++index)
        fixture.write(0x100U + index * 2U, 2, program[index]);
    const std::uint16_t handler[] = {0x33FC, 0xA55A, 0x0000, 0x0208, 0x4E73};
    for (unsigned index = 0; index < std::size(handler); ++index)
        fixture.write(0x180U + index * 2U, 2, handler[index]);
    saturn::platform::M68k cpu(fixture.bus());
    cpu.reset();
    REQUIRE(cpu.pc() == 0x100U);
    REQUIRE(cpu.execute(0U) == 0U);
    REQUIRE(cpu.stats().executeCalls == 0U);
    REQUIRE(cpu.execute(1U) == 1U);
    REQUIRE(cpu.pc() == 0x100U);
    REQUIRE(cpu.execute(39U) == 39U);
    REQUIRE(cpu.pc() == 0x100U);
    REQUIRE(cpu.execute(1000U) >= 1000U);
    REQUIRE(fixture.read(0x200U, 1) == 0x12U);
    REQUIRE(fixture.read(0x202U, 2) == 0x3456U);
    REQUIRE(fixture.read(0x204U, 4) == 0x789ABCDEU);
    REQUIRE(fixture.writes[0] != 0U);
    REQUIRE(fixture.writes[1] != 0U);
    REQUIRE(fixture.writes[2] != 0U);
    REQUIRE(fixture.read(0x208U, 2) == 0U);
    cpu.setIrq(2U);
    static_cast<void>(cpu.execute(128U));
    cpu.setIrq(0U);
    REQUIRE(fixture.read(0x208U, 2) == 0xA55AU);
}

void rejectsSimultaneousCoresAndAllowsRecreation() {
    BusFixture fixture;
    {
        saturn::platform::M68k first(fixture.bus());
        bool rejected{};
        try { saturn::platform::M68k second(fixture.bus()); }
        catch (const std::logic_error&) { rejected = true; }
        REQUIRE(rejected);
    }
    saturn::platform::M68k recreated(fixture.bus());
    fixture.write(0, 4, 0x8000U);
    fixture.write(4, 4, 0x100U);
    recreated.reset();
    REQUIRE(recreated.pc() == 0x100U);
}
} // namespace

void registerM68kTests(test::Registry& registry) {
    registry.add("M68k: real instructions, bus widths and IRQ", executesReal68000InstructionsAndInterrupt);
    registry.add("M68k: global core ownership and recreation", rejectsSimultaneousCoresAndAllowsRecreation);
}
