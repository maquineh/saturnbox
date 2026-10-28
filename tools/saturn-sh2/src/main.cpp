#include "saturn/core/memory_image.hpp"
#include "saturn/sh2/analysis.hpp"
#include "saturn/sh2/cfg.hpp"
#include "saturn/sh2/decoder.hpp"
#include "saturn/sh2/format.hpp"
#include "saturn/sh2/module_lifter.hpp"
#include "saturn/sh2/runtime.hpp"
#include "saturn/saturn/mmio.hpp"
#include "saturn/saturn/boot_services.hpp"
#include "saturn/saturn/scheduler.hpp"
#include "saturn/saturn/slave.hpp"
#include "saturn/sir/interpreter.hpp"
#include "saturn/sir/module.hpp"
#include "saturn/sir/static_module.hpp"
#include "saturn/sir/win64_jit.hpp"
#include "live_debug.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include "win32_input.hpp"
#endif

#include <algorithm>
#include <array>
#include <bitset>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(SATURNBOX_HAS_STATIC_MODULE)
extern "C" const saturn::sir::StaticNativeBlockEntry*
saturnbox_static_blocks(std::size_t* count) noexcept;
#endif

namespace {

[[nodiscard]] const saturn::sir::StaticNativeBlockEntry*
builtInStaticBlocks(std::size_t& count) noexcept {
#if defined(SATURNBOX_HAS_STATIC_MODULE)
    return saturnbox_static_blocks(&count);
#else
    count = 0U;
    return nullptr;
#endif
}

struct Arguments {
    std::string_view command;
    std::filesystem::path input;
    std::optional<std::filesystem::path> ipImage;
    std::optional<std::filesystem::path> program;
    std::optional<std::filesystem::path> discImage;
    std::optional<std::filesystem::path> resourcePath;
    std::optional<saturn::core::Address> resourceBase;
    std::optional<std::filesystem::path> frameDirectory;
    std::optional<std::filesystem::path> blackFrameProbe;
    std::optional<std::filesystem::path> vdp2MapTrace;
    std::optional<std::filesystem::path> vdp2MapReadTrace;
    std::optional<std::filesystem::path> audioOutput;
    std::optional<std::filesystem::path> scspRequestsOutput;
    std::optional<std::filesystem::path> aotEntryOutput;
    std::optional<std::filesystem::path> aotModuleOutput;
    std::optional<std::filesystem::path> controlTraceOutput;
    std::optional<std::filesystem::path> x4PlayerSamplesOutput;
    std::optional<std::filesystem::path> debugSessionDirectory;
    std::optional<std::string> padSequence;
    // Debug-only scripted memory writes, applied once per scheduler step:
    // step, address, value, width. Used to keep the player alive in long
    // automated routes; never used by the emulator itself.
    std::vector<std::tuple<std::size_t, saturn::core::Address, std::uint32_t, std::uint8_t>> pokes;
    std::optional<saturn::core::Address> watchAddress;
    std::optional<std::filesystem::path> watchDirectory;
    std::size_t watchLength{};
    std::size_t watchPeriod{};
    std::size_t watchFrom{};
    std::optional<std::size_t> padLiveAfterBlocks;
    std::optional<std::size_t> controlTraceFromStep;
    bool controlTraceOreg2Only{};
    std::size_t discSectorSize{2048U};
    saturn::core::Address base{};
    bool hasBase{};
    saturn::core::Address aipBase{};
    bool hasAipBase{};
    saturn::core::Address ipBase{};
    bool hasIpBase{};
    saturn::core::Address programBase{};
    bool hasProgramBase{};
    std::optional<saturn::core::Address> entry;
    std::optional<saturn::core::Address> expectedEntry;
    std::optional<std::filesystem::path> output;
    std::array<std::optional<std::string>, 2> padButtons;
    std::size_t padAfterBlocks{};
    std::optional<std::size_t> padReleaseAfterBlocks;
    std::optional<std::size_t> padSecondPressAfterBlocks;
    std::optional<std::size_t> padSecondReleaseAfterBlocks;
    std::optional<std::size_t> padThirdPressAfterBlocks;
    std::optional<std::size_t> padThirdReleaseAfterBlocks;
    std::optional<std::size_t> frameCaptureAfterBlocks;
    std::size_t maxSteps{1'000'000};
    std::size_t vblankPeriodBlocks{};
    bool stepsSpecified{};
    bool vblankSpecified{};
    bool trace{};
    bool traceCdSectors{};
    bool forceInterpreter{};
    bool traceGame{};
    bool traceForeground{};
    bool continueAfterExecuteDmp{};
    bool window{};
    bool audioLive{};
    bool scspDriver{};
    std::string_view renderer{"cpu"};
    bool rendererSpecified{};
    bool frameChangesOnly{};
};

struct MemoryProbeRecord {
    saturn::core::Address address{};
    std::uint8_t width{};
    std::uint32_t value{};
    bool write{};
    saturn::sir::ExecutionContext context;
};

struct SpriteSourceWriteRecord {
    std::size_t step{};
    saturn::core::Address block{};
    saturn::core::Address caller{};
    saturn::core::Address address{};
    saturn::core::Address physicalAddress{};
    std::uint8_t width{};
    std::uint32_t previousValue{};
    std::uint32_t value{};
    bool pointerWrite{};
};

struct Vdp2MapWriteRecord {
    MemoryProbeRecord access;
    std::uint32_t previousValue{};
};

struct GfsParserSourceReadRecord {
    MemoryProbeRecord access;
    std::uint32_t fifoOffsetBefore{};
    std::uint32_t fifoOffsetAfter{};
    std::uint32_t transferLba{};
    std::uint32_t transferOffset{};
    std::uint32_t transferCount{};
    std::uint8_t transferPartition{0xFFU};
    std::uint8_t transferInstruction{};
};

struct CdFifoReadRecord {
    std::size_t step{};
    saturn::core::Address block{};
    saturn::core::Address caller{};
    saturn::core::Address address{};
    std::uint8_t width{};
    std::uint32_t value{};
    std::uint32_t fifoOffsetBefore{};
    std::uint32_t fifoOffsetAfter{};
    std::uint32_t transferLba{};
    std::uint32_t transferOffset{};
    std::uint32_t transferCount{};
    std::uint8_t transferPartition{0xFFU};
    std::uint8_t transferInstruction{};
    std::array<std::uint32_t, 16> registers{};
};

struct BlockTransitionRecord {
    saturn::core::Address from{};
    saturn::core::Address to{};
    std::uint8_t eventValue{};
    saturn::sir::ExecutionContext context;
};

struct ResolverFlowRecord {
    saturn::core::Address block{};
    saturn::core::Address caller{};
    std::uint64_t step{};
    std::array<std::uint32_t, 16> regs{};
    std::string lookupKey;
    std::optional<std::uint8_t> overlayState;
    std::optional<std::uint8_t> firstNameIndex;
    std::optional<std::uint8_t> secondNameIndex;
};

struct TitleGfsSetupRecord {
    saturn::core::Address block{};
    saturn::core::Address caller{};
    std::uint64_t step{};
    saturn::core::Address handle{};
    std::uint32_t handleArgument{};
    std::uint32_t handleState{};
    std::uint32_t handleWordAt28{};
    std::array<std::uint32_t, 3> ringFields{};
    std::array<std::uint32_t, 16> regs{};
};

struct DispatcherDispatchRecord {
    std::uint32_t index{};
    saturn::core::Address table{};
    saturn::core::Address slot{};
    saturn::core::Address target{};
    saturn::core::Address caller{};
    std::size_t hits{};
    std::size_t firstStep{};
    std::size_t lastStep{};
    saturn::sir::ExecutionContext context;
};

struct ObjectCallbackDispatchRecord {
    saturn::core::Address caller{};
    saturn::core::Address callback{};
    saturn::core::Address object{};
    saturn::core::Address table{};
    saturn::core::Address slotByObjectId{};
    saturn::core::Address targetByObjectId{};
    saturn::core::Address slotByR9Id{};
    saturn::core::Address targetByR9Id{};
    saturn::core::Address r3Target{};
    std::uint8_t objectId{};
    std::uint8_t r9Id{};
    std::size_t hits{};
    std::size_t firstStep{};
    std::size_t lastStep{};
    std::array<std::uint32_t, 16> regs{};
};

struct ForegroundProbeRecord {
    saturn::core::Address block{};
    std::size_t hits{};
    std::size_t firstStep{};
    std::size_t lastStep{};
    std::array<std::uint32_t, 16> regs{};
    std::array<std::uint32_t, 4> pointedWords{}; // *r4, *r6, *r9, *r13
    std::array<std::uint32_t, 4> stackWords{};
    saturn::sir::ExecutionContext context;
};

struct TitleCallbackStateRecord {
    std::uint32_t index{};
    saturn::core::Address table{};
    saturn::core::Address slot{};
    saturn::core::Address target{};
    std::size_t hits{};
    std::size_t firstStep{};
    std::size_t lastStep{};
    saturn::sir::ExecutionContext context;
};

struct CdPlayCommandRecord {
    MemoryProbeRecord command;
    std::array<std::uint32_t, 16> stack{};
    std::uint64_t playCommands{};
    std::array<std::uint16_t, 4> playRegisters{};
    std::uint32_t titleStateIndex{};
    saturn::core::Address titleStateTarget{};
};

struct CdSectorRequestRecord {
    MemoryProbeRecord command;
    std::array<std::uint32_t, 16> stack{};
    std::uint64_t transferSectorRequests{};
    std::uint32_t transferLba{};
    std::uint32_t transferOffset{};
    std::uint32_t transferCount{};
    std::uint8_t transferPartition{0xFFU};
    std::uint8_t transferInstruction{};
};

struct GfsUpdateRecord {
    saturn::core::Address block{};
    saturn::core::Address object{};
    std::array<std::uint32_t, 5> stack{};
    std::uint32_t source{};
    std::uint32_t active{};
    std::uint32_t submitted{};
    std::uint32_t consumed{};
    std::uint32_t cursor{};
    std::uint32_t target{};
    saturn::core::Address stream{};
    std::array<std::uint32_t, 8> streamFields{};
    saturn::sir::ExecutionContext context;
};

struct SoundCommandRecord {
    saturn::core::Address packet{};
    std::array<std::uint32_t, 4> words{};
    saturn::sir::ExecutionContext context;
};

struct GfsProducerRecord {
    saturn::core::Address caller{};
    saturn::core::Address wrapper{};
    std::uint32_t produced{};
    saturn::core::Address object{};
    saturn::core::Address stream{};
    std::array<std::uint32_t, 8> streamFields{};
    saturn::sir::ExecutionContext context;
};

struct GfsProducerPathRecord {
    saturn::core::Address block{};
    saturn::core::Address caller{};
    saturn::core::Address controller{};
    saturn::core::Address table{};
    saturn::core::Address wrapper{};
    std::uint32_t result{};
    std::array<std::uint32_t, 7> controllerFields{};
    std::uint32_t descriptorCount{};
    saturn::core::Address descriptor{};
    std::array<std::uint32_t, 4> descriptorFields{};
    saturn::core::Address object{};
    saturn::core::Address stream{};
    // The allocator at 0x0603FC90 uses these ring fields directly; retain
    // them alongside the wider +84..+112 stream snapshot so a failed
    // descriptor acquisition can be explained without a post-run race.
    std::array<std::uint32_t, 5> ringFields{}; // +0,+4,+12,+20,+112
    std::uint32_t ringAllocationBase{}; // stream + 0x180
    std::uint32_t ringAllocationSlot{}; // base + 64 + index*16
    std::array<std::uint32_t, 8> streamFields{};
    std::array<std::uint32_t, 6> transferFields{};
    std::array<std::uint32_t, 2> pollOutputs{};
    std::size_t producerAdvances{};
    std::size_t cdCommands{};
    saturn::sir::ExecutionContext context;
};

struct GfsTransferPollRecord {
    saturn::core::Address block{};
    saturn::core::Address wrapper{};
    saturn::core::Address object{};
    std::array<std::uint32_t, 5> wrapperFields{}; // wrapper +0x194..+0x1A4
    std::array<std::uint32_t, 5> fields{}; // +0x194..+0x1A4
    std::array<std::uint32_t, 5> stack{};
    std::array<std::uint32_t, 6> regs{}; // r0,r4,r5,r6,r7,r13
    saturn::sir::ExecutionContext context;
};

struct GfsAllocatorRecord {
    saturn::core::Address block{};
    std::array<std::uint32_t, 8> regs{}; // r0,r1,r2,r3,r4,r5,r6,r14
    std::array<std::uint32_t, 6> stream{}; // +84,+88,+108,+112,+124,+128
    saturn::sir::ExecutionContext context;
};

struct GfsCapacityRecord {
    saturn::core::Address block{};
    std::array<std::uint32_t, 11> regs{}; // r0..r7,r13,r14,r15
    std::array<std::uint32_t, 8> stack{};
    saturn::sir::ExecutionContext context;
};

struct DmaDescriptorRecord {
    std::uint64_t step{};
    saturn::core::Address block{};
    saturn::core::Address caller{};
    saturn::core::Address descriptor{};
    std::array<std::uint32_t, 12> fields{};
    std::array<std::uint32_t, 6> sourceSamples{};
    saturn::sir::ExecutionContext context;
};

struct GfsSourceChunkRecord {
    std::size_t writes{};
    std::size_t nonZeroWrites{};
    std::uint64_t firstStep{};
    std::uint64_t lastStep{};
    saturn::core::Address firstAddress{};
    saturn::core::Address lastAddress{};
    std::uint32_t firstValue{};
    std::uint32_t lastValue{};
    saturn::core::Address firstBlock{};
    saturn::core::Address lastBlock{};
    saturn::core::Address firstCaller{};
    saturn::core::Address lastCaller{};
};

struct GfsDecodeRecord {
    std::uint64_t step{};
    saturn::core::Address block{};
    saturn::core::Address caller{};
    saturn::core::Address samplePointer{};
    std::array<std::uint32_t, 12> registers{};
    std::array<std::uint32_t, 8> inputWords{};
    std::array<std::uint32_t, 16> stackWords{};
    std::array<std::uint32_t, 10> taskObjectWords{};
    std::array<std::uint32_t, 4> parserParameterWords{};
    // R0/R1/R2/R3/R4/R5/R6/R13/R14, the four words selected by R5, and
    // the five fields read from @(12/20/100/108/112,R14) by 0x0604022C.
    std::array<std::uint32_t, 18> parserCalculationWords{};
    saturn::sir::ExecutionContext context;
    // Captured at 0x0604241A while the parser is still using the selected
    // STAB entry.  Reading this from RAM after execution would be racy because
    // the producer advances object+0x0C to the next entry later on.
    saturn::core::Address stabDescriptor{};
    std::array<std::uint32_t, 4> stabFields{};
    std::uint32_t parserObjectIndex{};
};

struct GfsSkipRunRecord {
    std::uint64_t step{};
    saturn::core::Address input{};
    saturn::core::Address output{};
    std::uint32_t command{};
    std::uint32_t innerRemaining{};
    std::uint32_t rowRemaining{};
    std::array<std::uint32_t, 4> inputWords{};
};

struct GfsOutputCoverageRecord {
    std::uint64_t step{};
    std::uint64_t videoSequence{};
    std::uint64_t preFrameHash{};
    std::uint64_t postFrameHash{};
    saturn::core::Address outputBase{};
    saturn::core::Address compressedSource{};
    std::uint32_t stride{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t rows{};
    std::array<std::uint16_t, 224> nonZeroWords{};
    std::array<std::uint16_t, 224> nonZeroRgbWords{};
    std::array<std::uint32_t, 224> firstWord{};
    std::array<std::uint32_t, 224> lastWord{};
    // FNV-1a over the complete 320-word row as observed at the decoder
    // boundary.  First/last samples can match even when the middle of a
    // line differs (important for detecting field/interlace expansion).
    std::array<std::uint32_t, 224> rowHash{};
    std::array<std::uint16_t, 224> writtenWords{};
    std::array<std::uint16_t, 224> writtenNonZeroWords{};
    std::array<std::uint16_t, 224> uniqueWrittenWords{};
    std::array<std::uint16_t, 224> changedWords{};
    std::array<std::uint32_t, 224> preRowHash{};
    std::vector<std::uint32_t> preFrameWords;
    std::vector<std::uint32_t> postFrameWords;
    std::uint32_t skippedUnchangedWords{};
    std::uint32_t skippedChangedWords{};
    std::uint32_t changedWordCount{};
    std::uint32_t writeCount{};
    std::uint32_t maxWrittenRow{};
    std::uint64_t maxWrittenOffset{};
    saturn::core::Address firstWrite{};
    saturn::core::Address lastWrite{};
};

constexpr std::array<std::uint32_t, 6> kDmaSourceSampleOffsets{
    0x45FFCU, 0x46000U, 0x46004U, 0x6FFFCU, 0x70000U, 0x70004U,
};
constexpr saturn::core::Address kGfsSourceBase = 0x060B49CCU;
constexpr std::size_t kGfsSourceSize = 0x70008U;
constexpr std::size_t kGfsSourceChunkSize = 0x1000U;
constexpr saturn::core::Address kGfsSourceBoundaryBegin = kGfsSourceBase + 0x45F00U;
constexpr saturn::core::Address kGfsSourceBoundaryEnd = kGfsSourceBase + 0x46020U;
constexpr saturn::core::Address kGfsTaskObjectAddress = 0x0606E45CU;
constexpr saturn::core::Address kGfsTaskConfigBegin = kGfsTaskObjectAddress + 0x58U;
constexpr saturn::core::Address kGfsTaskConfigEnd = kGfsTaskObjectAddress + 0x80U;
constexpr saturn::core::Address kGfsParserObjectAddress = 0x0607C038U;
constexpr saturn::core::Address kGfsParserTableEntryAddress = 0x0608021CU;
constexpr saturn::core::Address kGfsParserSourceAddress = 0x25818000U;

// Optional RAM probe used by launch --trace to verify that a CD FIFO sector
// survives the SH-2 copy routine.  Device accesses are still delegated to the
// normal boot-service/MMIO chain; only the small buffer range is recorded.
class LaunchMemoryProbe final : public saturn::sir::MemoryInterceptor {
public:
    LaunchMemoryProbe(saturn::platform::BootRomServices& services,
                      saturn::platform::MmioInterceptor& mmio,
                      const saturn::sir::MachineState& state,
                      bool cdFifoOnly = false)
        : services_(services), mmio_(mmio), state_(state),
          cdFifoOnly_(cdFifoOnly) {}

    void setExecutionContext(const saturn::sir::ExecutionContext& context) override {
        context_ = context;
        services_.setExecutionContext(context);
    }

    [[nodiscard]] saturn::core::Address backingAddress(
        saturn::core::Address address) const noexcept override {
        // Tracing must preserve the SH-2 RAM aliases used by guest code.
        return services_.backingAddress(address);
    }

    [[nodiscard]] std::optional<std::uint32_t> read(
        saturn::core::Address address, std::uint8_t width) override {
        const auto fifoOffsetBefore = mmio_.cd().fifoOffset();
        const auto value = services_.read(address, width);
        const auto fifoOffsetAfter = mmio_.cd().fifoOffset();
        if (cdFifoOnly_) {
            observeCdFifoRead(address, width, value.value_or(0U),
                              fifoOffsetBefore, fifoOffsetAfter);
            return value;
        }
        observeGfsParserSourceRead(address, width, value.value_or(0U),
                                   fifoOffsetBefore, fifoOffsetAfter);
        observeCdFifoRead(address, width, value.value_or(0U),
                          fifoOffsetBefore, fifoOffsetAfter);
        if (context_.blockAddress >= 0x06042B50U &&
            context_.blockAddress < 0x06042C4AU &&
            (address & 0x0FF00000U) == 0x05A00000U) {
            ++audioStatusReadCount_;
            lastAudioStatusRead_ = MemoryProbeRecord{
                address, width, value.value_or(0U), false, context_};
        }
        if (overlaps(address, width, kWaitEventAddress)) {
            ++waitEventReadCount_;
            appendRing(waitEventRecords_, waitEventRecordCount_,
                       MemoryProbeRecord{address, width,
                                         value.value_or(readRam(address, width)),
                                         false, context_});
        }
        if (titleQueueRequestSeen_ && overlapsTitleProgressState(address, width)) {
            appendRing(titleProgressStateReads_, titleProgressStateReadCount_,
                       MemoryProbeRecord{address, width,
                                         value.value_or(readRam(address, width)),
                                         false, context_});
        }
        if (context_.step >= kProbeStartStep &&
            ((address >= kProbeBegin && address < kProbeEnd) ||
             (address >= 0x06009780U && address < 0x060097B0U)) &&
            records_.size() < kProbeLimit) {
            // BootRomServices returns nullopt for ordinary sparse RAM; the
            // interpreter then supplies the byte(s) from MachineState. The
            // probe is interested in the address/context even in that case.
            records_.push_back({address, width, value.value_or(0U), false, context_});
        }
        return value;
    }

    [[nodiscard]] bool write(saturn::core::Address address, std::uint8_t width,
                             std::uint32_t value) override {
        if (cdFifoOnly_) return services_.write(address, width, value);
        const auto offset = address & 0x000FFFFFU;
        const auto isVdp2Map =
            saturn::platform::classify(address) == saturn::platform::AddressSpace::Vdp2 &&
            offset >= 0x70000U && offset < 0x72000U;
        const auto previousVdp2MapValue = isVdp2Map
            ? services_.read(address, width).value_or(0U) : 0U;
        if (titleQueueRequestSeen_ && overlapsTitleProgressState(address, width)) {
            appendRing(titleProgressStateWrites_, titleProgressStateWriteCount_,
                       MemoryProbeRecord{address, width, value, true, context_});
        }
        if (overlaps(address, width, 0x0606A87CU)) {
            appendRing(titleGfsHookWrites_, titleGfsHookWriteCount_,
                       MemoryProbeRecord{address, width, value, true, context_});
        }
        if (titleQueueRequestSeen_ &&
            (overlaps(address, width, 0x0606A858U) ||
             overlaps(address, width, 0x0606A85CU) ||
             overlaps(address, width, 0x0606A8B0U) ||
             overlaps(address, width, 0x0606A8B4U) ||
             overlaps(address, width, 0x0606A8B8U))) {
            appendRing(titleGfsMemberWrites_, titleGfsMemberWriteCount_,
                       MemoryProbeRecord{address, width, value, true, context_});
        }
        const auto handled = services_.write(address, width, value);
        if (isVdp2Map && vdp2MapWrites_.size() < 1'000'000U) {
            vdp2MapWrites_.push_back({{address, width, value, true, context_},
                                      previousVdp2MapValue});
        }
        observeGfsSourceWrite(address, width, value);
        observeGfsTaskConfigWrite(address, width, value);
        observeGfsParserStackWrite(address, width, value);
        observeGfsParserDataWrite(address, width, value);
        if (address >= 0x0606E608U && address < 0x0606E610U) {
            ++audioStatusCacheWriteCount_;
            if (audioStatusCacheWrites_.size() < 32U) {
                audioStatusCacheWrites_.push_back(
                    MemoryProbeRecord{address, width, value, true, context_});
            }
        }
        if ((address & 0x1FFFFFFFU) == 0x05A004E0U && (value & 0x80U) != 0U) {
            SoundCommandRecord record{readRam(0x06051948U, 4U), {}, context_};
            for (std::size_t index = 0; index < record.words.size(); ++index) {
                record.words[index] = services_.read(
                    record.packet + static_cast<std::uint32_t>(index * 4U), 4U).value_or(0U);
            }
            appendRing(soundCommands_, soundCommandCount_, std::move(record));
        }
        if (audioController_ != 0U && address >= audioController_ + 0x80U &&
            address < audioController_ + 0xB0U && readRam(address, width) != value) {
            appendRing(audioCursorWrites_, audioCursorWriteCount_,
                       MemoryProbeRecord{address, width, value, true, context_});
        }
        if ((address == 0x05890018U || address == 0x25890018U) && value != 0U) {
            const auto stackPointer = registerValue(saturn::sh2::Register::R15);
            // 0x0603554C is nested below the CD command wrapper and its
            // interrupt-safe bridge. At this point SP+44 holds the return
            // address of the high-level CDC/GFS caller.
            const MemoryProbeRecord record{readRam(stackPointer + 44U, 4U),
                                            width, value, true, context_};
            appendRing(cdCommandRecords_, cdCommandRecordCount_, record);
            if (value == 0x00004401U) {
                CdPlayCommandRecord playRecord{record, {}};
                for (std::size_t index = 0; index < playRecord.stack.size(); ++index) {
                    playRecord.stack[index] = readRam(
                        static_cast<saturn::core::Address>(
                            stackPointer + static_cast<std::uint32_t>(index * 4U)),
                        4U);
                }
                const auto& cdStats = mmio_.cd().stats();
                playRecord.playCommands = cdStats.playCommands;
                playRecord.playRegisters = cdStats.lastPlayRegisters;
                playRecord.titleStateIndex = readRam(0x060519CCU, 4U);
                if (playRecord.titleStateIndex < 9U) {
                    const auto stateSlot = static_cast<saturn::core::Address>(
                        0x060460E8U + playRecord.titleStateIndex * 4U);
                    playRecord.titleStateTarget = readRam(stateSlot, 4U);
                }
                appendRing(cdPlayCommandRecords_, cdPlayCommandRecordCount_,
                           std::move(playRecord));
            }
            if (titleQueueRequestSeen_ &&
                (value == 0x00006100U || value == 0x00006300U)) {
                CdSectorRequestRecord sectorRecord{record, {}};
                for (std::size_t index = 0; index < sectorRecord.stack.size(); ++index) {
                    sectorRecord.stack[index] = readRam(
                        static_cast<saturn::core::Address>(
                            stackPointer + static_cast<std::uint32_t>(index * 4U)),
                        4U);
                }
                const auto& cdStats = mmio_.cd().stats();
                sectorRecord.transferSectorRequests = cdStats.transferSectorRequests;
                sectorRecord.transferLba = cdStats.lastTransferLba;
                sectorRecord.transferOffset = cdStats.lastTransferOffset;
                sectorRecord.transferCount = cdStats.lastTransferCount;
                sectorRecord.transferPartition = cdStats.lastTransferPartition;
                sectorRecord.transferInstruction = cdStats.lastTransferInstruction;
                appendRing(cdSectorRequestRecords_, cdSectorRequestRecordCount_,
                           std::move(sectorRecord));
            }
        }
        const auto eventWrite =
            address == 0x06051940U || address == 0x06051944U ||
            address == 0x06051948U || address == 0x060519A4U ||
            address == kWaitEventAddress ||
            overlaps(address, width, kTitleWaitFlagAddress) ||
            address == 0x0606C355U || address == 0x0606C498U;
        if (eventWrite) {
            appendRing(eventRecords_, eventRecordCount_,
                       MemoryProbeRecord{address, width, value, true, context_});
        }
        if (overlaps(address, width, kTitleWaitFlagAddress)) {
            appendRing(titleWaitFlagWrites_, titleWaitFlagWriteCount_,
                       MemoryProbeRecord{address, width, value, true, context_});
            if (context_.blockAddress == 0x0601CE50U && (value & 0xFFU) != 0U) {
                titleQueueRequestSeen_ = true;
            }
        }
        if (overlaps(address, width, kTitleQueueIndexAddress) ||
            overlaps(address, width, kTitleQueueCountAddress) ||
            overlaps(address, width, kTitleWaitFlagAddress) ||
            (static_cast<std::uint64_t>(address) < kTitleQueueEntriesEnd &&
             static_cast<std::uint64_t>(address) + width > kTitleQueueEntriesBegin)) {
            appendRing(titleQueueWrites_, titleQueueWriteCount_,
                       MemoryProbeRecord{address, width, value, true, context_});
        }
        if (overlaps(address, width, kWaitEventAddress)) {
            ++waitEventWriteCount_;
            const MemoryProbeRecord record{address, width, value, true, context_};
            appendRing(waitEventRecords_, waitEventRecordCount_, record);
            appendRing(waitEventWrites_, waitEventWriteRecordCount_, record);
        }
        if (context_.step >= kProbeStartStep &&
            ((address >= 0x06009780U && address < 0x060097B0U) || eventWrite) &&
            records_.size() < kProbeLimit) {
            records_.push_back({address, width, value, true, context_});
        }
        return handled;
    }

    [[nodiscard]] const std::vector<MemoryProbeRecord>& records() const noexcept {
        return records_;
    }
    [[nodiscard]] const std::vector<Vdp2MapWriteRecord>& vdp2MapWrites() const noexcept {
        return vdp2MapWrites_;
    }
    [[nodiscard]] const std::vector<MemoryProbeRecord>& eventRecords() const noexcept {
        return eventRecords_;
    }
    [[nodiscard]] std::size_t eventRecordCount() const noexcept {
        return eventRecordCount_;
    }
    [[nodiscard]] const std::vector<MemoryProbeRecord>& titleWaitFlagWrites() const noexcept {
        return titleWaitFlagWrites_;
    }
    [[nodiscard]] std::size_t titleWaitFlagWriteCount() const noexcept {
        return titleWaitFlagWriteCount_;
    }
    [[nodiscard]] const std::vector<MemoryProbeRecord>& titleQueueWrites() const noexcept {
        return titleQueueWrites_;
    }
    [[nodiscard]] std::size_t titleQueueWriteCount() const noexcept {
        return titleQueueWriteCount_;
    }
    [[nodiscard]] const std::vector<MemoryProbeRecord>&
    titleProgressStateReads() const noexcept {
        return titleProgressStateReads_;
    }
    [[nodiscard]] std::size_t titleProgressStateReadCount() const noexcept {
        return titleProgressStateReadCount_;
    }
    [[nodiscard]] const std::vector<MemoryProbeRecord>&
    titleProgressStateWrites() const noexcept {
        return titleProgressStateWrites_;
    }
    [[nodiscard]] std::size_t titleProgressStateWriteCount() const noexcept {
        return titleProgressStateWriteCount_;
    }
    [[nodiscard]] const std::vector<ResolverFlowRecord>& resolverFlowRecords() const noexcept {
        return resolverFlowRecords_;
    }
    [[nodiscard]] std::size_t resolverFlowRecordCount() const noexcept {
        return resolverFlowRecordCount_;
    }
    [[nodiscard]] const std::vector<ResolverFlowRecord>&
    stageArchiveFlowRecords() const noexcept {
        return stageArchiveFlowRecords_;
    }
    [[nodiscard]] std::size_t stageArchiveFlowRecordCount() const noexcept {
        return stageArchiveFlowRecordCount_;
    }
    [[nodiscard]] const std::vector<TitleGfsSetupRecord>&
    titleGfsSetupRecords() const noexcept {
        return titleGfsSetupRecords_;
    }
    [[nodiscard]] const std::vector<MemoryProbeRecord>&
    titleGfsHookWrites() const noexcept {
        return titleGfsHookWrites_;
    }
    [[nodiscard]] std::size_t titleGfsHookWriteCount() const noexcept {
        return titleGfsHookWriteCount_;
    }
    [[nodiscard]] const std::vector<MemoryProbeRecord>&
    titleGfsMemberWrites() const noexcept {
        return titleGfsMemberWrites_;
    }
    [[nodiscard]] std::size_t titleGfsMemberWriteCount() const noexcept {
        return titleGfsMemberWriteCount_;
    }
    [[nodiscard]] const std::vector<MemoryProbeRecord>& waitEventRecords() const noexcept {
        return waitEventRecords_;
    }
    [[nodiscard]] std::size_t waitEventRecordCount() const noexcept {
        return waitEventRecordCount_;
    }
    [[nodiscard]] std::size_t waitEventReadCount() const noexcept {
        return waitEventReadCount_;
    }
    [[nodiscard]] std::size_t waitEventWriteCount() const noexcept {
        return waitEventWriteCount_;
    }
    [[nodiscard]] std::size_t audioStatusCacheWriteCount() const noexcept {
        return audioStatusCacheWriteCount_;
    }
    [[nodiscard]] const std::vector<MemoryProbeRecord>& audioStatusCacheWrites() const noexcept {
        return audioStatusCacheWrites_;
    }
    [[nodiscard]] const std::vector<MemoryProbeRecord>& waitEventWrites() const noexcept {
        return waitEventWrites_;
    }
    [[nodiscard]] std::uint8_t waitEventValue() const noexcept {
        return static_cast<std::uint8_t>(readRam(kWaitEventAddress, 1U));
    }
    [[nodiscard]] std::uint32_t ram32(saturn::core::Address address) const noexcept {
        return readRam(address, 4U);
    }
    [[nodiscard]] saturn::core::Address resolvedTaskAddress() const noexcept {
        return resolvedTaskAddress_;
    }
    [[nodiscard]] const std::vector<MemoryProbeRecord>&
    cdCommandRecords() const noexcept {
        return cdCommandRecords_;
    }
    [[nodiscard]] std::size_t cdCommandRecordCount() const noexcept {
        return cdCommandRecordCount_;
    }
    [[nodiscard]] const std::vector<CdPlayCommandRecord>&
    cdPlayCommandRecords() const noexcept {
        return cdPlayCommandRecords_;
    }
    [[nodiscard]] const std::vector<CdSectorRequestRecord>&
    cdSectorRequestRecords() const noexcept {
        return cdSectorRequestRecords_;
    }
    [[nodiscard]] std::size_t cdSectorRequestRecordCount() const noexcept {
        return cdSectorRequestRecordCount_;
    }

    void observeBlockTransition() {
        const auto blockAddress = context_.blockAddress;
        observeObjectCallbackDispatch(blockAddress);
        observeTitleGfsSetupBlock(blockAddress);
        const auto isTitleWorkerBlock =
            titleQueueRequestSeen_ && blockAddress >= 0x0601C000U &&
            blockAddress < 0x0601D000U;
        if (isResolverFlowCheckpoint(blockAddress) || isTitleWorkerBlock) {
            constexpr std::array<saturn::sh2::Register, 16> kRegisters{
                saturn::sh2::Register::R0, saturn::sh2::Register::R1,
                saturn::sh2::Register::R2, saturn::sh2::Register::R3,
                saturn::sh2::Register::R4, saturn::sh2::Register::R5,
                saturn::sh2::Register::R6, saturn::sh2::Register::R7,
                saturn::sh2::Register::R8, saturn::sh2::Register::R9,
                saturn::sh2::Register::R10, saturn::sh2::Register::R11,
                saturn::sh2::Register::R12, saturn::sh2::Register::R13,
                saturn::sh2::Register::R14, saturn::sh2::Register::R15,
            };
            ResolverFlowRecord record{context_.blockAddress, context_.callerBlock,
                                      context_.step, {}};
            for (std::size_t index = 0; index < kRegisters.size(); ++index) {
                record.regs[index] = registerValue(kRegisters[index]);
            }
            if (blockAddress == 0x0603AD4AU) {
                // The matcher preserves the key in R5; R1 points into the
                // caller's stack frame and is not a filename string.
                record.lookupKey = readRamString(record.regs[5], 32U);
            } else if (blockAddress == 0x002719E8U) {
                // R12 still points to the template at this block entry; the
                // two dynamic characters are written later in the block.
                record.lookupKey = readRamString(record.regs[12], 16U);
            } else if (blockAddress == 0x0601CE2AU) {
                // JSR's delay slot moves the filename from R12 to R4.
                record.lookupKey = readRamString(record.regs[4], 16U);
            }
            bool isStageArchiveStateTransition = false;
            if (blockAddress == 0x0027196AU ||
                blockAddress == 0x0027197AU) {
                const auto object = static_cast<saturn::core::Address>(
                    record.regs[14]);
                const auto state = static_cast<std::uint8_t>(
                    readRam(object + 5U, 1U));
                const auto [found, inserted] =
                    stageOverlayStates_.try_emplace(object, state);
                if (inserted || found->second != state) {
                    found->second = state;
                    record.overlayState = state;
                    const auto stageData = static_cast<saturn::core::Address>(
                        record.regs[13]);
                    record.firstNameIndex = static_cast<std::uint8_t>(
                        readRam(stageData + 12U, 1U));
                    record.secondNameIndex = static_cast<std::uint8_t>(
                        readRam(stageData + 13U, 1U));
                    isStageArchiveStateTransition = true;
                }
            }
            const auto isStageArchiveCall =
                blockAddress == 0x002719E8U ||
                (blockAddress == 0x0601CE2AU &&
                 (context_.callerBlock == 0x002719E8U ||
                  record.lookupKey.rfind("ST0_1_", 0U) == 0U));
            const auto isStageArchiveResolverMatch =
                blockAddress == 0x0603AD4AU &&
                (record.lookupKey == "ST0_1_0.ARC" ||
                 record.lookupKey == "ST0_1_1.ARC");
            if (isStageArchiveCall) {
                const auto object = static_cast<saturn::core::Address>(
                    record.regs[14]);
                const auto stageData = static_cast<saturn::core::Address>(
                    record.regs[13]);
                record.overlayState = static_cast<std::uint8_t>(
                    readRam(object + 5U, 1U));
                record.firstNameIndex = static_cast<std::uint8_t>(
                    readRam(stageData + 12U, 1U));
                record.secondNameIndex = static_cast<std::uint8_t>(
                    readRam(stageData + 13U, 1U));
            }
            if (isStageArchiveStateTransition || isStageArchiveCall ||
                isStageArchiveResolverMatch) {
                appendRing(stageArchiveFlowRecords_, stageArchiveFlowRecordCount_,
                           record);
            }
            appendRing(resolverFlowRecords_, resolverFlowRecordCount_,
                       std::move(record));
        }
        if (context_.blockAddress == 0x06016EDAU) {
            const auto indexPointer = static_cast<saturn::core::Address>(
                registerValue(saturn::sh2::Register::R14));
            const auto index = readRam(indexPointer, 4U);
            const auto table = static_cast<saturn::core::Address>(
                registerValue(saturn::sh2::Register::R13));
            const auto slot = static_cast<saturn::core::Address>(table + index * 4U);
            const auto target = readRam(slot, 4U);
            const auto found = std::find_if(
                titleCallbackStateRecords_.begin(), titleCallbackStateRecords_.end(),
                [index, target](const TitleCallbackStateRecord& record) {
                    return record.index == index && record.target == target;
                });
            if (found == titleCallbackStateRecords_.end()) {
                titleCallbackStateRecords_.push_back(
                    {index, table, slot, target, 1U, context_.step,
                     context_.step, context_});
            } else {
                ++found->hits;
                found->lastStep = context_.step;
                found->context = context_;
            }
        }

        constexpr std::array<saturn::core::Address, 56> kForegroundProbeBlocks{
            0x06014046U, 0x060140F0U, 0x06014174U, 0x0601417CU,
            0x06014724U, 0x0601472EU, 0x06013AFAU, 0x06014734U,
            0x0601473EU, 0x06014748U, 0x060148E8U, 0x0022750CU,
            0x0601507CU, 0x060152EEU, 0x0601530CU, 0x06015366U,
            0x06015378U, 0x06038DA8U, 0x06038E56U, 0x06034A2CU,
            0x06015188U, 0x06015296U, 0x060152CCU, 0x0603D238U,
            0x0603DC6EU, 0x0603D91CU, 0x06041622U,
            0x0603859EU, 0x06038616U, 0x0603864CU, 0x06038674U,
            0x0603867EU, 0x06038684U, 0x0603886CU, 0x06038890U,
            0x0603E444U, 0x0603C03CU, 0x060390A6U, 0x0603B566U,
            0x0603C800U, 0x0603C80CU, 0x0603C846U, 0x0603C898U,
            0x0603D1DEU, 0x0603DB4AU, 0x0603DB68U,
            0x06037B88U, 0x0603D770U, 0x0603D7B2U, 0x0603D7C4U,
            0x060382EEU, 0x06038330U, 0x060387A0U, 0x06038822U,
            0x0603B636U, 0x0603B80EU,
        };
        if (std::find(kForegroundProbeBlocks.begin(), kForegroundProbeBlocks.end(),
                      context_.blockAddress) != kForegroundProbeBlocks.end()) {
            constexpr std::array<saturn::sh2::Register, 16> kRegisters{
                saturn::sh2::Register::R0, saturn::sh2::Register::R1,
                saturn::sh2::Register::R2, saturn::sh2::Register::R3,
                saturn::sh2::Register::R4, saturn::sh2::Register::R5,
                saturn::sh2::Register::R6, saturn::sh2::Register::R7,
                saturn::sh2::Register::R8, saturn::sh2::Register::R9,
                saturn::sh2::Register::R10, saturn::sh2::Register::R11,
                saturn::sh2::Register::R12, saturn::sh2::Register::R13,
                saturn::sh2::Register::R14, saturn::sh2::Register::R15,
            };
            ForegroundProbeRecord record{};
            record.block = context_.blockAddress;
            record.hits = 1U;
            record.firstStep = context_.step;
            record.lastStep = context_.step;
            record.context = context_;
            for (std::size_t index = 0; index < kRegisters.size(); ++index) {
                record.regs[index] = registerValue(kRegisters[index]);
            }
            constexpr std::array<std::size_t, 4> kPointerRegisterIndices{4U, 6U, 9U, 13U};
            for (std::size_t index = 0; index < kPointerRegisterIndices.size(); ++index) {
                const auto address = static_cast<saturn::core::Address>(
                    record.regs[kPointerRegisterIndices[index]]);
                record.pointedWords[index] = readRam(address, 4U);
                record.stackWords[index] = readRam(static_cast<saturn::core::Address>(
                    record.regs[15] + index * 4U), 4U);
            }
            const auto found = std::find_if(
                foregroundProbeRecords_.begin(), foregroundProbeRecords_.end(),
                [&record](const ForegroundProbeRecord& previous) {
                    const bool splitFile = record.block == 0x0603C800U ||
                        record.block == 0x0603C80CU || record.block == 0x0603C846U ||
                        record.block == 0x0603C898U;
                    return previous.block == record.block &&
                        (!splitFile || previous.regs[14] == record.regs[14]);
                });
            if (found == foregroundProbeRecords_.end()) {
                foregroundProbeRecords_.push_back(std::move(record));
            } else {
                ++found->hits;
                found->lastStep = record.lastStep;
                found->regs = record.regs;
                found->pointedWords = record.pointedWords;
                found->stackWords = record.stackWords;
                found->context = record.context;
            }
        }
        if (context_.blockAddress == 0x0603084CU) {
            const auto offset = registerValue(saturn::sh2::Register::R0);
            const auto table = readRam(0x06030868U, 4U);
            const auto slot = static_cast<saturn::core::Address>(table + offset);
            const auto target = readRam(slot, 4U);
            const auto index = offset / 4U;
            const auto found = std::find_if(
                dispatcherDispatches_.begin(), dispatcherDispatches_.end(),
                [index, target](const DispatcherDispatchRecord& record) {
                    return record.index == index && record.target == target;
                });
            if (found == dispatcherDispatches_.end()) {
                dispatcherDispatches_.push_back({index, table, slot, target,
                                                 context_.callerBlock, 1U,
                                                 context_.step, context_.step,
                                                 context_});
            } else {
                ++found->hits;
                found->lastStep = context_.step;
                found->caller = context_.callerBlock;
                found->context = context_;
            }
        }
        if (context_.blockAddress == 0x0603FC90U ||
            context_.blockAddress == 0x0603FD04U ||
            context_.blockAddress == 0x0603FD44U ||
            context_.blockAddress == 0x0603FD50U ||
            context_.blockAddress == 0x0603FD5AU ||
            context_.blockAddress == 0x0603FD8EU ||
            context_.blockAddress == 0x0603FDC8U) {
            const auto stream = static_cast<saturn::core::Address>(
                registerValue(saturn::sh2::Register::R14));
            GfsAllocatorRecord record{context_.blockAddress,
                                      {registerValue(saturn::sh2::Register::R0),
                                       registerValue(saturn::sh2::Register::R1),
                                       registerValue(saturn::sh2::Register::R2),
                                       registerValue(saturn::sh2::Register::R3),
                                       registerValue(saturn::sh2::Register::R4),
                                       registerValue(saturn::sh2::Register::R5),
                                       registerValue(saturn::sh2::Register::R6),
                                       registerValue(saturn::sh2::Register::R14)},
                                      {readRam(stream + 84U, 4U),
                                       readRam(stream + 88U, 4U),
                                       readRam(stream + 108U, 4U),
                                       readRam(stream + 112U, 4U),
                                       readRam(stream + 124U, 4U),
                                       readRam(stream + 128U, 4U)},
                                      context_};
            appendRing(gfsAllocatorRecords_, gfsAllocatorRecordCount_,
                       std::move(record));
        }
        if (context_.blockAddress == 0x0603FDBCU ||
            context_.blockAddress == 0x0603FDC8U ||
            context_.blockAddress == 0x0603FDECU ||
            context_.blockAddress == 0x0603FE00U) {
            GfsCapacityRecord record{context_.blockAddress, {}, {}, context_};
            record.regs = {registerValue(saturn::sh2::Register::R0),
                           registerValue(saturn::sh2::Register::R1),
                           registerValue(saturn::sh2::Register::R2),
                           registerValue(saturn::sh2::Register::R3),
                           registerValue(saturn::sh2::Register::R4),
                           registerValue(saturn::sh2::Register::R5),
                           registerValue(saturn::sh2::Register::R6),
                           registerValue(saturn::sh2::Register::R7),
                           registerValue(saturn::sh2::Register::R13),
                           registerValue(saturn::sh2::Register::R14),
                           registerValue(saturn::sh2::Register::R15)};
            const auto sp = static_cast<saturn::core::Address>(record.regs[10]);
            for (std::size_t index = 0; index < record.stack.size(); ++index) {
                record.stack[index] = readRam(
                    static_cast<saturn::core::Address>(sp + index * 4U), 4U);
            }
            appendRing(gfsCapacityRecords_, gfsCapacityRecordCount_,
                       std::move(record));
        }
        if (context_.blockAddress == 0x0603A740U ||
            context_.blockAddress == 0x0603A750U ||
            context_.blockAddress == 0x0603A75CU ||
            context_.blockAddress == 0x0603A79EU ||
            context_.blockAddress == 0x0603A6FCU) {
            const auto wrapper = registerValue(saturn::sh2::Register::R14);
            const auto object = readRam(wrapper, 4U);
            GfsTransferPollRecord record{context_.blockAddress, wrapper, object, {}, {}, {}, {}, context_};
            for (std::size_t index = 0; index < record.wrapperFields.size(); ++index) {
                record.wrapperFields[index] = readRam(static_cast<saturn::core::Address>(
                    wrapper + 0x194U + static_cast<std::uint32_t>(index * 4U)), 4U);
            }
            for (std::size_t index = 0; index < record.fields.size(); ++index) {
                record.fields[index] = readRam(static_cast<saturn::core::Address>(
                    object + 0x194U + static_cast<std::uint32_t>(index * 4U)), 4U);
            }
            const auto sp = registerValue(saturn::sh2::Register::R15);
            for (std::size_t index = 0; index < record.stack.size(); ++index) {
                record.stack[index] = readRam(static_cast<saturn::core::Address>(
                    sp + static_cast<std::uint32_t>(index * 4U)), 4U);
            }
            record.regs = {registerValue(saturn::sh2::Register::R0),
                           registerValue(saturn::sh2::Register::R4),
                           registerValue(saturn::sh2::Register::R5),
                           registerValue(saturn::sh2::Register::R6),
                           registerValue(saturn::sh2::Register::R7),
                           registerValue(saturn::sh2::Register::R13)};
            appendRing(gfsTransferPolls_, gfsTransferPollCount_, std::move(record));
        }
        if (context_.blockAddress == 0x06042B50U) ++audioStatusPolls_;
        if (context_.blockAddress == 0x06042AA6U) ++audioStatusDispatches_;
        if (context_.blockAddress == 0x06042AFCU) ++audioConsumerAdvances_;
        if (context_.blockAddress == 0x060430CCU) {
            const auto wrapper = registerValue(saturn::sh2::Register::R4);
            audioController_ = readRam(wrapper, 4U) + 0x38U;
        }
        if (context_.blockAddress == 0x0603FFF4U) {
            lastFirstProducerExit_ = context_;
            const auto sp = registerValue(saturn::sh2::Register::R15);
            for (std::size_t index = 0; index < firstProducerExitStack_.size(); ++index) {
                firstProducerExitStack_[index] = readRam(
                    static_cast<saturn::core::Address>(sp + index * 4U), 4U);
            }
        }
        if (context_.blockAddress == 0x06034A84U) {
            const auto descriptor = static_cast<saturn::core::Address>(
                registerValue(saturn::sh2::Register::R4));
            DmaDescriptorRecord record{
                context_.step,
                context_.blockAddress,
                context_.callerBlock,
                descriptor,
                {},
                {},
                context_,
            };
            for (std::size_t index = 0; index < record.fields.size(); ++index) {
                record.fields[index] = readRam(static_cast<saturn::core::Address>(
                    descriptor + static_cast<std::uint32_t>(index * 4U)), 4U);
            }
            const auto source = record.fields[0];
            for (std::size_t index = 0; index < kDmaSourceSampleOffsets.size(); ++index) {
                record.sourceSamples[index] = readRam(static_cast<saturn::core::Address>(
                    source + kDmaSourceSampleOffsets[index]), 4U);
            }
            if (record.fields[1] == 0x25E00000U && dmaDescriptorRecords_.size() < 256U) {
                dmaDescriptorRecords_.push_back(std::move(record));
            }
        }
        const auto gfsOutput = registerValue(saturn::sh2::Register::R4);
        const auto gfsOutputInWindow =
            gfsOutput >= kGfsSourceBase &&
            gfsOutput < kGfsSourceBase + 0x46020U;
        const auto gfsAtOutputBoundary =
            gfsOutput <= kGfsSourceBase + 0x200U ||
            gfsOutput >= kGfsSourceBase + 0x45C00U;
        const auto gfsLoopBlock =
            context_.blockAddress == 0x0604195CU ||
            context_.blockAddress == 0x06041D90U ||
            context_.blockAddress == 0x06041F56U;
        const auto gfsSkipBlock = context_.blockAddress == 0x0604205EU;
        if (gfsSkipBlock && gfsSkipRecords_.size() < kGfsSkipTraceLimit) {
            GfsSkipRunRecord record{
                context_.step,
                registerValue(saturn::sh2::Register::R14),
                registerValue(saturn::sh2::Register::R4),
                registerValue(saturn::sh2::Register::R10),
                registerValue(saturn::sh2::Register::R5),
                registerValue(saturn::sh2::Register::R7),
                {},
            };
            for (std::size_t index = 0; index < record.inputWords.size(); ++index) {
                record.inputWords[index] = readRam(static_cast<saturn::core::Address>(
                    record.input + static_cast<std::uint32_t>(index * 4U)), 4U);
            }
            gfsSkipRecords_.push_back(std::move(record));
        }
        const auto gfsSetupBlock =
            context_.blockAddress == 0x06041D14U ||
            context_.blockAddress == 0x06041D3EU ||
            context_.blockAddress == 0x06041D42U ||
            context_.blockAddress == 0x06041D46U ||
            context_.blockAddress == 0x06041D48U ||
            context_.blockAddress == 0x06041D50U ||
            context_.blockAddress == 0x06041D5EU ||
            context_.blockAddress == 0x06041D8AU ||
            context_.blockAddress == 0x06041D30U ||
            context_.blockAddress == 0x06041ED0U ||
            context_.blockAddress == 0x06041E74U ||
            context_.blockAddress == 0x06041E8EU ||
            context_.blockAddress == 0x06041EB6U ||
            context_.blockAddress == 0x06042088U ||
            context_.blockAddress == 0x06041E9CU ||
            context_.blockAddress == 0x06041EA2U ||
            context_.blockAddress == 0x06041EA6U ||
            context_.blockAddress == 0x06042092U;
        const auto gfsTaskConfigBlock =
            context_.blockAddress == 0x06042698U ||
            context_.blockAddress == 0x06042702U;
        const auto gfsParserBlock =
            context_.blockAddress == 0x06040454U ||
            context_.blockAddress == 0x0604045EU ||
            context_.blockAddress == 0x0604241AU ||
            context_.blockAddress == 0x06042418U ||
            context_.blockAddress == 0x06042438U ||
            context_.blockAddress == 0x06042478U ||
            context_.blockAddress == 0x0604248CU ||
            context_.blockAddress == 0x060424D8U ||
            context_.blockAddress == 0x06042520U ||
            context_.blockAddress == 0x06042530U ||
            context_.blockAddress == 0x06042590U ||
            context_.blockAddress == 0x060425C2U ||
            context_.blockAddress == 0x060425FCU ||
            context_.blockAddress == 0x06042736U;
        const auto gfsCalculationBlock =
            context_.blockAddress == 0x0604022CU ||
            context_.blockAddress == 0x06040246U ||
            context_.blockAddress == 0x06040256U ||
            context_.blockAddress == 0x06040262U ||
            context_.blockAddress == 0x06040284U ||
            context_.blockAddress == 0x0604029AU ||
            context_.blockAddress == 0x0604029CU ||
            context_.blockAddress == 0x060402A0U;
        const auto gfsBoundaryBlock = gfsSetupBlock || gfsTaskConfigBlock ||
                                      gfsParserBlock;
        const auto captureGfsDecode =
            gfsLoopBlock || gfsTaskConfigBlock || gfsParserBlock ||
            gfsCalculationBlock ||
            (gfsSetupBlock && gfsOutputInWindow && gfsAtOutputBoundary);
        if (captureGfsDecode &&
            ((gfsLoopBlock && gfsDecodeRecords_.size() < 512U) ||
             (gfsBoundaryBlock &&
              gfsDecodeBoundaryRecords_.size() < kGfsDecodeBoundaryTraceLimit) ||
             (gfsCalculationBlock && gfsParserCalculationRecords_.size() < 512U))) {
            constexpr std::array<saturn::sh2::Register, 12> kDecodeRegisters{
                saturn::sh2::Register::R4, saturn::sh2::Register::R5,
                saturn::sh2::Register::R6, saturn::sh2::Register::R7,
                saturn::sh2::Register::R8, saturn::sh2::Register::R9,
                saturn::sh2::Register::R10, saturn::sh2::Register::R11,
                saturn::sh2::Register::R12, saturn::sh2::Register::R13,
                saturn::sh2::Register::R14, saturn::sh2::Register::R15,
            };
            const auto samplePointer =
                (context_.blockAddress == 0x0604195CU ||
                 context_.blockAddress == 0x06042418U ||
                 context_.blockAddress == 0x0604241AU)
                    ? registerValue(saturn::sh2::Register::R4)
                    : gfsParserBlock
                        ? registerValue(saturn::sh2::Register::R13)
                        : registerValue(saturn::sh2::Register::R14);
            GfsDecodeRecord record{
                context_.step,
                context_.blockAddress,
                context_.callerBlock,
                samplePointer,
                {},
                {},
                {},
                {},
                {},
                {},
                context_,
            };
            for (std::size_t index = 0; index < kDecodeRegisters.size(); ++index) {
                record.registers[index] = registerValue(kDecodeRegisters[index]);
            }
            const auto input = record.samplePointer;
            for (std::size_t index = 0; index < record.inputWords.size(); ++index) {
                record.inputWords[index] = readRam(static_cast<saturn::core::Address>(
                    input + static_cast<std::uint32_t>(index * 4U)), 4U);
            }
            const auto stackPointer =
                registerValue(saturn::sh2::Register::R15);
            for (std::size_t index = 0; index < record.stackWords.size(); ++index) {
                record.stackWords[index] = readRam(static_cast<saturn::core::Address>(
                    stackPointer + static_cast<std::uint32_t>(index * 4U)), 4U);
            }
            if (context_.blockAddress == 0x06041D14U) {
                const auto object = record.registers[10];
                for (std::size_t index = 0; index < record.taskObjectWords.size(); ++index) {
                    record.taskObjectWords[index] = readRam(static_cast<saturn::core::Address>(
                        object + 0x58U + static_cast<std::uint32_t>(index * 4U)), 4U);
                }
            }
            if (context_.blockAddress == 0x0604045EU) {
                const auto r9 = record.registers[5];
                const auto r13 = record.registers[9];
                record.parserParameterWords[0] = readRam(r9 + 12U, 4U);
                record.parserParameterWords[1] = readRam(r9 + 16U, 4U);
                record.parserParameterWords[2] = readRam(r13 + 36U, 1U);
                record.parserParameterWords[3] = readRam(
                    static_cast<saturn::core::Address>(record.registers[11] + 4U), 4U);
            }
            if (context_.blockAddress == 0x0604241AU) {
                // At this callback R13 is the parser table and R14 is the
                // parser object. Capture the index and the exact STAB words
                // before the producer advances object+0x0C to the next entry.
                const auto table = static_cast<saturn::core::Address>(record.registers[9]);
                const auto object = static_cast<saturn::core::Address>(record.registers[10]);
                record.parserObjectIndex = readRam(object + 0x0CU, 4U);
                if (table != 0U &&
                    record.parserObjectIndex <=
                        (std::numeric_limits<std::uint32_t>::max() - 64U) / 16U) {
                    record.stabDescriptor = static_cast<saturn::core::Address>(
                        table + 64U + record.parserObjectIndex * 16U);
                    for (std::size_t index = 0; index < record.stabFields.size(); ++index) {
                        record.stabFields[index] = readRam(
                            static_cast<saturn::core::Address>(
                                record.stabDescriptor + index * 4U), 4U);
                    }
                }
            }
            if (gfsCalculationBlock) {
                constexpr std::array<saturn::sh2::Register, 9> kCalculationRegisters{
                    saturn::sh2::Register::R0, saturn::sh2::Register::R1,
                    saturn::sh2::Register::R2, saturn::sh2::Register::R3,
                    saturn::sh2::Register::R4, saturn::sh2::Register::R5,
                    saturn::sh2::Register::R6, saturn::sh2::Register::R13,
                    saturn::sh2::Register::R14,
                };
                for (std::size_t index = 0; index < kCalculationRegisters.size(); ++index) {
                    record.parserCalculationWords[index] =
                        registerValue(kCalculationRegisters[index]);
                }
                const auto object = record.parserCalculationWords[8];
                constexpr std::array<std::uint32_t, 5> kCalculationFieldOffsets{
                    12U, 20U, 100U, 108U, 112U};
                for (std::size_t index = 0; index < kCalculationFieldOffsets.size(); ++index) {
                    record.parserCalculationWords[13U + index] = readRam(
                        static_cast<saturn::core::Address>(
                            object + kCalculationFieldOffsets[index]), 4U);
                }
                // R5 is the table-entry pointer only after 0x0604023C.  The
                // snapshots at 0x0604029A..0x060402A0 therefore expose the
                // four words consumed by the subsequent loads.
                if (context_.blockAddress == 0x0604029AU ||
                    context_.blockAddress == 0x0604029CU ||
                    context_.blockAddress == 0x060402A0U) {
                    const auto table = record.parserCalculationWords[5];
                    for (std::size_t index = 0; index < 4U; ++index) {
                        record.parserCalculationWords[9U + index] = readRam(
                            static_cast<saturn::core::Address>(table + index * 4U), 4U);
                    }
                }
            }
            if (context_.blockAddress == 0x06041D14U) {
                activeGfsOutputBase_ = static_cast<saturn::core::Address>(
                    record.registers[0]);
                activeGfsOutputStride_ = record.registers[1];
                activeGfsOutputWidth_ = record.registers[2];
                activeGfsOutputHeight_ = record.registers[3];
                activeGfsCompressedSource_ = record.taskObjectWords[6];
                activeGfsOutputCoverage_ = true;
                activeGfsWrittenWords_.fill(0U);
                activeGfsWrittenNonZeroWords_.fill(0U);
                for (auto& row : activeGfsWrittenMask_) row.reset();
                const auto rows = std::min<std::uint32_t>(
                    activeGfsOutputHeight_ * 2U, 224U);
                const auto words = std::min<std::uint32_t>(
                    activeGfsOutputWidth_, 0x140U);
                activeGfsPreOutput_.assign(
                    static_cast<std::size_t>(rows) * words, 0U);
                for (std::uint32_t row = 0; row < rows; ++row) {
                    for (std::uint32_t word = 0; word < words; ++word) {
                        activeGfsPreOutput_[static_cast<std::size_t>(row) * words + word] =
                            readRam(static_cast<saturn::core::Address>(
                                activeGfsOutputBase_ + row * activeGfsOutputStride_ + word * 4U), 4U);
                    }
                }
                activeGfsPreFrameHash_ = 14695981039346656037ULL;
                for (const auto value : activeGfsPreOutput_) {
                    activeGfsPreFrameHash_ =
                        (activeGfsPreFrameHash_ ^ value) * 1099511628211ULL;
                }
                activeGfsWriteCount_ = 0U;
                activeGfsMaxWrittenRow_ = 0U;
                activeGfsMaxWrittenOffset_ = 0U;
                activeGfsFirstWrite_ = 0U;
                activeGfsLastWrite_ = 0U;
            } else if (context_.blockAddress == 0x06041EB6U) {
                // Alternate four-plane decoder.  Unlike 0x06041D14 it has
                // no task object argument, so capture the live registers.
                activeGfsOutputBase_ = static_cast<saturn::core::Address>(
                    record.registers[0]);
                activeGfsOutputStride_ = record.registers[1];
                activeGfsOutputWidth_ = record.registers[2];
                activeGfsOutputHeight_ = record.registers[3];
                activeGfsCompressedSource_ = static_cast<saturn::core::Address>(
                    record.registers[10]);
                activeGfsOutputCoverage_ = true;
                activeGfsWrittenWords_.fill(0U);
                activeGfsWrittenNonZeroWords_.fill(0U);
                for (auto& row : activeGfsWrittenMask_) row.reset();
                const auto rows = std::min<std::uint32_t>(
                    activeGfsOutputHeight_ * 2U, 224U);
                const auto words = std::min<std::uint32_t>(
                    activeGfsOutputWidth_, 0x140U);
                activeGfsPreOutput_.assign(
                    static_cast<std::size_t>(rows) * words, 0U);
                for (std::uint32_t row = 0; row < rows; ++row) {
                    for (std::uint32_t word = 0; word < words; ++word) {
                        activeGfsPreOutput_[static_cast<std::size_t>(row) * words + word] =
                            readRam(static_cast<saturn::core::Address>(
                                activeGfsOutputBase_ + row * activeGfsOutputStride_ + word * 4U), 4U);
                    }
                }
                activeGfsPreFrameHash_ = 14695981039346656037ULL;
                for (const auto value : activeGfsPreOutput_) {
                    activeGfsPreFrameHash_ =
                        (activeGfsPreFrameHash_ ^ value) * 1099511628211ULL;
                }
                activeGfsWriteCount_ = 0U;
                activeGfsMaxWrittenRow_ = 0U;
                activeGfsMaxWrittenOffset_ = 0U;
                activeGfsFirstWrite_ = 0U;
                activeGfsLastWrite_ = 0U;
            }
            if (gfsLoopBlock) {
                gfsDecodeRecords_.push_back(record);
            }
            if (gfsBoundaryBlock) {
                gfsDecodeBoundaryRecords_.push_back(record);
            }
            if (gfsCalculationBlock) {
                gfsParserCalculationRecords_.push_back(std::move(record));
            }
        }
        const auto gfsReturnOutput = registerValue(saturn::sh2::Register::R4);
        const auto gfsReturnAtBoundary =
            gfsReturnOutput <= kGfsSourceBase + 0x200U ||
            gfsReturnOutput >= kGfsSourceBase + 0x45C00U;
        if ((context_.blockAddress == 0x06041EA6U ||
             context_.blockAddress == 0x06042092U) &&
            gfsReturnAtBoundary &&
            activeGfsOutputBase_ != 0U &&
            gfsOutputCoverageRecords_.size() < kGfsOutputCoverageLimit) {
            GfsOutputCoverageRecord coverage{};
            coverage.step = context_.step;
            coverage.videoSequence = mmio_.video().lastFrame().sequence;
            coverage.preFrameHash = activeGfsPreFrameHash_;
            coverage.postFrameHash = 14695981039346656037ULL;
            coverage.outputBase = activeGfsOutputBase_;
            coverage.compressedSource = activeGfsCompressedSource_;
            coverage.stride = activeGfsOutputStride_;
            coverage.width = activeGfsOutputWidth_;
            coverage.height = activeGfsOutputHeight_;
            coverage.rows = std::min<std::uint32_t>(
                activeGfsOutputHeight_ * 2U,
                static_cast<std::uint32_t>(224U));
            coverage.writeCount = activeGfsWriteCount_;
            coverage.maxWrittenRow = activeGfsMaxWrittenRow_;
            coverage.maxWrittenOffset = activeGfsMaxWrittenOffset_;
            coverage.firstWrite = activeGfsFirstWrite_;
            coverage.lastWrite = activeGfsLastWrite_;
            coverage.writtenWords = activeGfsWrittenWords_;
            coverage.writtenNonZeroWords = activeGfsWrittenNonZeroWords_;
            for (std::size_t row = 0; row < coverage.uniqueWrittenWords.size(); ++row) {
                coverage.uniqueWrittenWords[row] = static_cast<std::uint16_t>(
                    activeGfsWrittenMask_[row].count());
            }
            const auto rowWordCount = std::min<std::uint32_t>(
                activeGfsOutputWidth_, 0x140U);
            const auto captureFrameWords = gfsOutputCoverageRecords_.size() < 2U;
            if (captureFrameWords) {
                coverage.preFrameWords = activeGfsPreOutput_;
                coverage.postFrameWords.reserve(
                    static_cast<std::size_t>(coverage.rows) * rowWordCount);
            }
            for (std::uint32_t row = 0; row < coverage.rows; ++row) {
                const auto rowBase = static_cast<saturn::core::Address>(
                    coverage.outputBase + row * coverage.stride);
                std::uint32_t nonZero{};
                std::uint32_t rowHash = 2166136261U;
                std::uint32_t preRowHash = 2166136261U;
                for (std::uint32_t word = 0; word < rowWordCount; ++word) {
                    const auto value = readRam(static_cast<saturn::core::Address>(
                        rowBase + word * 4U), 4U);
                    const auto preIndex = static_cast<std::size_t>(row) * rowWordCount + word;
                    const auto preValue = preIndex < activeGfsPreOutput_.size()
                        ? activeGfsPreOutput_[preIndex]
                        : 0U;
                    if (captureFrameWords) coverage.postFrameWords.push_back(value);
                    preRowHash ^= preValue;
                    preRowHash *= 16777619U;
                    if (value != preValue) {
                        ++coverage.changedWords[row];
                        ++coverage.changedWordCount;
                    }
                    coverage.postFrameHash =
                        (coverage.postFrameHash ^ value) * 1099511628211ULL;
                    if (!activeGfsWrittenMask_[row].test(word)) {
                        if (preIndex < activeGfsPreOutput_.size() &&
                            activeGfsPreOutput_[preIndex] == value) {
                            ++coverage.skippedUnchangedWords;
                        } else {
                            ++coverage.skippedChangedWords;
                        }
                    }
                    rowHash ^= value;
                    rowHash *= 16777619U;
                    if (value != 0U) {
                        ++nonZero;
                        if (coverage.firstWord[row] == 0U) {
                            coverage.firstWord[row] = value;
                        }
                        coverage.lastWord[row] = value;
                    }
                    if ((value & 0x00FFFFFFU) != 0U) {
                        ++coverage.nonZeroRgbWords[row];
                    }
                }
                coverage.nonZeroWords[row] = static_cast<std::uint16_t>(nonZero);
                coverage.rowHash[row] = rowHash;
                coverage.preRowHash[row] = preRowHash;
            }
            gfsOutputCoverageRecords_.push_back(std::move(coverage));
            activeGfsOutputCoverage_ = false;
        }
        const auto fromTarget = targetIndex(context_.callerBlock);
        const auto toTarget = targetIndex(context_.blockAddress);
        if (const auto checkpoint = gfsCheckpointIndex(context_.blockAddress)) {
            ++gfsCheckpointHits_[*checkpoint];
            const auto wrapper = registerValue(
                context_.blockAddress == 0x0603A646U
                    ? saturn::sh2::Register::R4
                    : saturn::sh2::Register::R14);
            const auto object = readRam(wrapper, 4U);
            const auto stackPointer = registerValue(saturn::sh2::Register::R15);
            // GFS passes the wrapper to 0x0603ED82; that routine dereferences
            // it to this object and uses the embedded ring at object+0x38.
            const auto stream = static_cast<saturn::core::Address>(object + 56U);
            GfsUpdateRecord record{context_.blockAddress, object, {},
                                   readRam(object + 0x188U, 4U),
                                   readRam(object + 0x194U, 4U),
                                   readRam(object + 0x198U, 4U),
                                   readRam(object + 0x19CU, 4U),
                                   readRam(object + 0x1A0U, 4U),
                                   readRam(object + 0x1A4U, 4U), stream, {}, context_};
            for (std::size_t index = 0; index < record.stack.size(); ++index) {
                record.stack[index] = readRam(static_cast<saturn::core::Address>(
                    stackPointer + index * 4U), 4U);
            }
            for (std::size_t index = 0; index < record.streamFields.size(); ++index) {
                record.streamFields[index] = readRam(static_cast<saturn::core::Address>(
                    stream + 84U + index * 4U), 4U);
            }
            lastGfsCheckpoint_[*checkpoint] = std::move(record);
        }
        if (context_.blockAddress == 0x0603EED6U) {
            const auto wrapper = static_cast<saturn::core::Address>(
                registerValue(saturn::sh2::Register::R4));
            const auto object = static_cast<saturn::core::Address>(readRam(wrapper, 4U));
            const auto stream = static_cast<saturn::core::Address>(object + 56U);
            GfsProducerRecord record{
                context_.callerBlock,
                wrapper,
                registerValue(saturn::sh2::Register::R5),
                object,
                stream,
                {},
                context_,
            };
            for (std::size_t index = 0; index < record.streamFields.size(); ++index) {
                record.streamFields[index] = readRam(static_cast<saturn::core::Address>(
                    stream + 84U + index * 4U), 4U);
            }
            appendRing(gfsProducerRecords_, gfsProducerRecordCount_, std::move(record));
        }
        if (isGfsProducerPathCheckpoint(context_.blockAddress, context_.callerBlock)) {
            const auto controller = static_cast<saturn::core::Address>(
                registerValue(saturn::sh2::Register::R14));
            const auto table = static_cast<saturn::core::Address>(
                registerValue(saturn::sh2::Register::R11));
            const auto wrapper = static_cast<saturn::core::Address>(
                registerValue(saturn::sh2::Register::R13));
            GfsProducerPathRecord record{
                context_.blockAddress,
                context_.callerBlock,
                controller,
                table,
                wrapper,
                registerValue(saturn::sh2::Register::R0),
                {},
                readRam(static_cast<saturn::core::Address>(table + 60U), 4U),
                0U,
                {},
                static_cast<saturn::core::Address>(readRam(wrapper, 4U)),
                0U,
                {},
                {},
                {},
                {},
                {},
                {},
                gfsProducerRecordCount_,
                cdCommandRecordCount_,
                context_,
            };
            constexpr std::array<saturn::core::Address, 7> kControllerOffsets{
                4U, 12U, 20U, 32U, 36U, 44U, 48U,
            };
            for (std::size_t index = 0; index < record.controllerFields.size(); ++index) {
                record.controllerFields[index] = readRam(static_cast<saturn::core::Address>(
                    controller + kControllerOffsets[index]), 4U);
            }
            const auto descriptorIndex = record.controllerFields[1];
            if (table != 0U && descriptorIndex < record.descriptorCount &&
                descriptorIndex <= (std::numeric_limits<std::uint32_t>::max() - 64U) / 16U) {
                record.descriptor = static_cast<saturn::core::Address>(
                    table + 64U + descriptorIndex * 16U);
                for (std::size_t index = 0; index < record.descriptorFields.size(); ++index) {
                    record.descriptorFields[index] = readRam(
                        static_cast<saturn::core::Address>(record.descriptor + index * 4U),
                        4U);
                }
            }
            record.stream = static_cast<saturn::core::Address>(record.object + 56U);
            constexpr std::array<saturn::core::Address, 5> kRingOffsets{
                0U, 4U, 12U, 20U, 112U,
            };
            for (std::size_t index = 0; index < record.ringFields.size(); ++index) {
                record.ringFields[index] = readRam(
                    static_cast<saturn::core::Address>(
                        record.stream + kRingOffsets[index]),
                    4U);
            }
            // 0x0603FC90 loads R10 from object+0x180 (before advancing R14
            // to the embedded stream at object+0x38), not from stream+0x180.
            record.ringAllocationBase = readRam(
                static_cast<saturn::core::Address>(record.object + 0x180U), 4U);
            const auto ringSlot = static_cast<saturn::core::Address>(
                record.ringAllocationBase + 64U + record.controllerFields[1] * 16U);
            record.ringAllocationSlot = readRam(ringSlot, 4U);
            for (std::size_t index = 0; index < record.streamFields.size(); ++index) {
                record.streamFields[index] = readRam(static_cast<saturn::core::Address>(
                    record.stream + 84U + index * 4U), 4U);
            }
            constexpr std::array<saturn::core::Address, 5> kTransferOffsets{
                0x8CU, 0x9CU, 0xA0U, 0x120U, 0x124U,
            };
            for (std::size_t index = 0; index < kTransferOffsets.size(); ++index) {
                record.transferFields[index] = readRam(
                    static_cast<saturn::core::Address>(
                        record.stream + kTransferOffsets[index]),
                    4U);
            }
            record.transferFields[5] = readRam(
                static_cast<saturn::core::Address>(record.object + 48U), 4U);
            const auto stackPointer = static_cast<saturn::core::Address>(
                registerValue(saturn::sh2::Register::R15));
            record.pollOutputs[0] = readRam(stackPointer, 4U);
            record.pollOutputs[1] = readRam(
                static_cast<saturn::core::Address>(stackPointer + 4U), 4U);
            appendGfsPathRecord(std::move(record));
        }
        if (context_.blockAddress == 0x0603A076U) {
            const auto found = context_.state.find(saturn::sh2::stateSlot(
                saturn::sh2::Register::R4));
            if (found != context_.state.end()) resolvedTaskAddress_ = found->second;
        }
        if (toTarget) ++targetHits_[*toTarget];
        if (context_.callerBlock == kCallbackAddress ||
            context_.blockAddress == kCallbackAddress) {
            appendRing(callbackTransitions_, callbackTransitionCount_,
                       BlockTransitionRecord{context_.callerBlock, context_.blockAddress,
                                             static_cast<std::uint8_t>(
                                                 readRam(kWaitEventAddress, 1U)),
                                             context_});
        }
        if (isAsyncTarget(context_.callerBlock) ||
            isAsyncTarget(context_.blockAddress)) {
            appendRing(asyncTransitions_, asyncTransitionCount_,
                       BlockTransitionRecord{context_.callerBlock, context_.blockAddress,
                                             static_cast<std::uint8_t>(
                                                 readRam(kWaitEventAddress, 1U)),
                                             context_});
        }
        if (context_.callerBlock == context_.blockAddress) return;
        if (!fromTarget && !toTarget) return;
        appendRing(blockTransitions_, blockTransitionCount_,
                   BlockTransitionRecord{context_.callerBlock, context_.blockAddress,
                                         static_cast<std::uint8_t>(
                                             readRam(kWaitEventAddress, 1U)),
                                         context_});
    }

    [[nodiscard]] const std::vector<BlockTransitionRecord>& blockTransitions() const noexcept {
        return blockTransitions_;
    }
    [[nodiscard]] const std::vector<DispatcherDispatchRecord>&
    dispatcherDispatches() const noexcept {
        return dispatcherDispatches_;
    }
    [[nodiscard]] const std::vector<ObjectCallbackDispatchRecord>&
    objectCallbackDispatches() const noexcept {
        return objectCallbackDispatches_;
    }
    [[nodiscard]] const std::vector<ForegroundProbeRecord>&
    foregroundProbeRecords() const noexcept {
        return foregroundProbeRecords_;
    }
    [[nodiscard]] const std::vector<TitleCallbackStateRecord>&
    titleCallbackStateRecords() const noexcept {
        return titleCallbackStateRecords_;
    }
    [[nodiscard]] std::size_t blockTransitionCount() const noexcept {
        return blockTransitionCount_;
    }
    [[nodiscard]] const std::vector<BlockTransitionRecord>&
    callbackTransitions() const noexcept {
        return callbackTransitions_;
    }
    [[nodiscard]] std::size_t callbackTransitionCount() const noexcept {
        return callbackTransitionCount_;
    }
    [[nodiscard]] const std::vector<BlockTransitionRecord>&
    asyncTransitions() const noexcept {
        return asyncTransitions_;
    }
    [[nodiscard]] std::size_t asyncTransitionCount() const noexcept {
        return asyncTransitionCount_;
    }
    [[nodiscard]] const std::array<std::size_t, 33>& targetHits() const noexcept {
        return targetHits_;
    }
    [[nodiscard]] static constexpr const std::array<saturn::core::Address, 33>&
    targetAddresses() noexcept {
        return kTargetAddresses;
    }
    [[nodiscard]] const std::array<std::size_t, 13>&
    gfsCheckpointHits() const noexcept {
        return gfsCheckpointHits_;
    }
    [[nodiscard]] const std::array<std::optional<GfsUpdateRecord>, 13>&
    lastGfsCheckpoints() const noexcept {
        return lastGfsCheckpoint_;
    }
    [[nodiscard]] const std::vector<GfsProducerRecord>&
    gfsProducerRecords() const noexcept {
        return gfsProducerRecords_;
    }
    [[nodiscard]] const std::optional<saturn::sir::ExecutionContext>&
    lastFirstProducerExit() const noexcept { return lastFirstProducerExit_; }
    [[nodiscard]] const std::array<std::uint32_t, 4>&
    firstProducerExitStack() const noexcept { return firstProducerExitStack_; }
    [[nodiscard]] saturn::core::Address audioController() const noexcept {
        return audioController_;
    }
    [[nodiscard]] const std::vector<MemoryProbeRecord>& audioCursorWrites() const noexcept {
        return audioCursorWrites_;
    }
    [[nodiscard]] std::size_t audioCursorWriteCount() const noexcept {
        return audioCursorWriteCount_;
    }
    [[nodiscard]] const std::vector<SoundCommandRecord>& soundCommands() const noexcept {
        return soundCommands_;
    }
    [[nodiscard]] std::size_t soundCommandCount() const noexcept { return soundCommandCount_; }
    [[nodiscard]] std::array<std::size_t, 4> audioStatusCounts() const noexcept {
        return {audioStatusPolls_, audioStatusDispatches_,
                audioConsumerAdvances_, audioStatusReadCount_};
    }
    [[nodiscard]] const std::optional<MemoryProbeRecord>& lastAudioStatusRead() const noexcept {
        return lastAudioStatusRead_;
    }
    [[nodiscard]] std::size_t gfsProducerRecordCount() const noexcept {
        return gfsProducerRecordCount_;
    }
    [[nodiscard]] const std::vector<GfsProducerPathRecord>&
    gfsProducerPathRecords() const noexcept {
        return gfsProducerPathRecords_;
    }
    [[nodiscard]] std::size_t gfsProducerPathRecordCount() const noexcept {
        return gfsProducerPathRecordCount_;
    }
    [[nodiscard]] const std::vector<GfsTransferPollRecord>&
    gfsTransferPolls() const noexcept { return gfsTransferPolls_; }
    [[nodiscard]] std::size_t gfsTransferPollCount() const noexcept {
        return gfsTransferPollCount_;
    }
    [[nodiscard]] const std::vector<GfsAllocatorRecord>&
    gfsAllocatorRecords() const noexcept { return gfsAllocatorRecords_; }
    [[nodiscard]] std::size_t gfsAllocatorRecordCount() const noexcept {
        return gfsAllocatorRecordCount_;
    }
    [[nodiscard]] const std::vector<GfsCapacityRecord>&
    gfsCapacityRecords() const noexcept { return gfsCapacityRecords_; }
    [[nodiscard]] std::size_t gfsCapacityRecordCount() const noexcept {
        return gfsCapacityRecordCount_;
    }
    [[nodiscard]] const std::vector<DmaDescriptorRecord>&
    dmaDescriptorRecords() const noexcept {
        return dmaDescriptorRecords_;
    }
    [[nodiscard]] const std::vector<GfsSourceChunkRecord>&
    gfsSourceChunks() const noexcept {
        return gfsSourceChunks_;
    }
    [[nodiscard]] const std::vector<MemoryProbeRecord>&
    gfsSourceBoundaryWrites() const noexcept {
        return gfsSourceBoundaryWrites_;
    }
    [[nodiscard]] const std::vector<MemoryProbeRecord>&
    gfsTaskConfigWrites() const noexcept {
        return gfsTaskConfigWrites_;
    }
    [[nodiscard]] const std::vector<MemoryProbeRecord>&
    gfsParserStackWrites() const noexcept {
        return gfsParserStackWrites_;
    }
    [[nodiscard]] const std::vector<MemoryProbeRecord>&
    gfsParserDataWrites() const noexcept {
        return gfsParserDataWrites_;
    }
    [[nodiscard]] const std::vector<GfsParserSourceReadRecord>&
    gfsParserSourceReads() const noexcept {
        return gfsParserSourceReads_;
    }
    [[nodiscard]] const std::vector<CdFifoReadRecord>&
    cdFifoReadRecords() const noexcept {
        return cdFifoReadRecords_;
    }
    [[nodiscard]] std::size_t gfsSourceWriteCount() const noexcept {
        return gfsSourceWriteCount_;
    }
    [[nodiscard]] std::size_t gfsSourceNonZeroWriteCount() const noexcept {
        return gfsSourceNonZeroWriteCount_;
    }
    [[nodiscard]] const std::vector<GfsDecodeRecord>&
    gfsDecodeRecords() const noexcept {
        return gfsDecodeRecords_;
    }
    [[nodiscard]] const std::vector<GfsDecodeRecord>&
    gfsDecodeBoundaryRecords() const noexcept {
        return gfsDecodeBoundaryRecords_;
    }
    [[nodiscard]] const std::vector<GfsDecodeRecord>&
    gfsParserCalculationRecords() const noexcept {
        return gfsParserCalculationRecords_;
    }
    [[nodiscard]] const std::vector<GfsOutputCoverageRecord>&
    gfsOutputCoverageRecords() const noexcept {
        return gfsOutputCoverageRecords_;
    }
    [[nodiscard]] const std::vector<GfsSkipRunRecord>&
    gfsSkipRecords() const noexcept {
        return gfsSkipRecords_;
    }
    [[nodiscard]] static constexpr const std::array<saturn::core::Address, 13>&
    gfsCheckpointAddresses() noexcept {
        return kGfsCheckpointAddresses;
    }

private:
    void observeObjectCallbackDispatch(saturn::core::Address callback) {
        const auto caller = context_.callerBlock;
        if (caller != 0x002223E2U && caller != 0x00222402U) return;

        constexpr std::array<saturn::sh2::Register, 16> kRegisters{
            saturn::sh2::Register::R0, saturn::sh2::Register::R1,
            saturn::sh2::Register::R2, saturn::sh2::Register::R3,
            saturn::sh2::Register::R4, saturn::sh2::Register::R5,
            saturn::sh2::Register::R6, saturn::sh2::Register::R7,
            saturn::sh2::Register::R8, saturn::sh2::Register::R9,
            saturn::sh2::Register::R10, saturn::sh2::Register::R11,
            saturn::sh2::Register::R12, saturn::sh2::Register::R13,
            saturn::sh2::Register::R14, saturn::sh2::Register::R15,
        };
        std::array<std::uint32_t, 16> regs{};
        for (std::size_t index = 0; index < kRegisters.size(); ++index) {
            regs[index] = registerValue(kRegisters[index]);
        }
        const auto object = static_cast<saturn::core::Address>(regs[4]);
        const auto table = static_cast<saturn::core::Address>(regs[12]);
        const auto objectId = static_cast<std::uint8_t>(
            readRam(static_cast<saturn::core::Address>(object + 1U), 1U));
        const auto r9Object = static_cast<saturn::core::Address>(regs[9]);
        const auto r9Id = static_cast<std::uint8_t>(
            readRam(static_cast<saturn::core::Address>(r9Object + 1U), 1U));
        const auto slotByObjectId = static_cast<saturn::core::Address>(
            table + static_cast<std::uint32_t>(objectId) * 4U);
        const auto slotByR9Id = static_cast<saturn::core::Address>(
            table + static_cast<std::uint32_t>(r9Id) * 4U);
        const auto targetByObjectId = static_cast<saturn::core::Address>(
            readRam(slotByObjectId, 4U));
        const auto targetByR9Id = static_cast<saturn::core::Address>(
            readRam(slotByR9Id, 4U));

        // The caller context also points to ordinary fall-through/return
        // blocks. Keep only entries where both object IDs, the function-table
        // target, R3, and the resolved block agree on the dispatched callback.
        if (objectId != r9Id || targetByObjectId != callback ||
            targetByR9Id != callback || regs[3] != callback) {
            return;
        }

        const auto found = std::find_if(
            objectCallbackDispatches_.begin(), objectCallbackDispatches_.end(),
            [caller, callback, objectId, r9Id, targetByObjectId, targetByR9Id]
            (const ObjectCallbackDispatchRecord& record) {
                return record.caller == caller && record.callback == callback &&
                       record.objectId == objectId && record.r9Id == r9Id &&
                       record.targetByObjectId == targetByObjectId &&
                       record.targetByR9Id == targetByR9Id;
            });
        if (found == objectCallbackDispatches_.end()) {
            if (objectCallbackDispatches_.size() >= 4096U) return;
            objectCallbackDispatches_.push_back({
                caller, callback, object, table, slotByObjectId,
                targetByObjectId, slotByR9Id, targetByR9Id,
                static_cast<saturn::core::Address>(regs[3]), objectId, r9Id,
                1U, context_.step, context_.step, regs});
        } else {
            ++found->hits;
            found->object = object;
            found->table = table;
            found->slotByObjectId = slotByObjectId;
            found->targetByObjectId = targetByObjectId;
            found->slotByR9Id = slotByR9Id;
            found->targetByR9Id = targetByR9Id;
            found->r3Target = static_cast<saturn::core::Address>(regs[3]);
            found->lastStep = context_.step;
            found->regs = regs;
        }
    }

    template <typename Record>
    static void appendRing(std::vector<Record>& records, std::size_t& count,
                           Record record) {
        if (records.size() < kTargetTraceLimit) {
            records.push_back(std::move(record));
        } else {
            records[count % kTargetTraceLimit] = std::move(record);
        }
        ++count;
    }

    void appendGfsPathRecord(GfsProducerPathRecord record) {
        if (gfsProducerPathRecords_.size() < kGfsPathTraceLimit) {
            gfsProducerPathRecords_.push_back(std::move(record));
        } else {
            gfsProducerPathRecords_[gfsProducerPathRecordCount_ % kGfsPathTraceLimit] =
                std::move(record);
        }
        ++gfsProducerPathRecordCount_;
    }

    void observeGfsSourceWrite(saturn::core::Address address,
                               std::uint8_t width,
                               std::uint32_t value) {
        const auto writeBegin = static_cast<std::uint64_t>(address);
        const auto writeEnd = writeBegin + width;
        const auto sourceBegin = static_cast<std::uint64_t>(kGfsSourceBase);
        const auto sourceEnd = sourceBegin + kGfsSourceSize;
        if (writeEnd <= sourceBegin || writeBegin >= sourceEnd) return;
        ++gfsSourceWriteCount_;
        if (value != 0U) ++gfsSourceNonZeroWriteCount_;

        // Keep a per-decompressor-call write map.  The final memory scan below
        // can see stale bytes left by an earlier resource; these counters only
        // include writes observed after the current decoder entry.
        if (activeGfsOutputCoverage_ && activeGfsOutputBase_ != 0U &&
            activeGfsOutputStride_ != 0U) {
            const auto outputBegin = static_cast<std::uint64_t>(activeGfsOutputBase_);
            const auto outputEnd = outputBegin +
                static_cast<std::uint64_t>(activeGfsOutputStride_) *
                std::min<std::uint32_t>(activeGfsOutputHeight_ * 2U, 224U);
            const auto clippedOutputBegin = std::max(writeBegin, outputBegin);
            const auto clippedOutputEnd = std::min(writeEnd, outputEnd);
            if (clippedOutputBegin < clippedOutputEnd) {
                if (activeGfsWriteCount_ == 0U) {
                    activeGfsFirstWrite_ = static_cast<saturn::core::Address>(
                        clippedOutputBegin);
                }
                activeGfsLastWrite_ = static_cast<saturn::core::Address>(
                    clippedOutputEnd - 1U);
                const auto firstWord = clippedOutputBegin & ~std::uint64_t{3U};
                const auto lastWord = (clippedOutputEnd - 1U) & ~std::uint64_t{3U};
                for (auto wordAddress = firstWord; wordAddress <= lastWord;
                     wordAddress += 4U) {
                    const auto offset = wordAddress - outputBegin;
                    const auto row = static_cast<std::size_t>(
                        offset / activeGfsOutputStride_);
                    if (row >= activeGfsWrittenWords_.size()) continue;
                    activeGfsMaxWrittenRow_ = std::max<std::uint32_t>(
                        activeGfsMaxWrittenRow_, static_cast<std::uint32_t>(row));
                    activeGfsMaxWrittenOffset_ = std::max(
                        activeGfsMaxWrittenOffset_, offset);
                    const auto rowOffset = offset % activeGfsOutputStride_;
                    if (rowOffset / 4U >= activeGfsOutputWidth_) continue;
                    ++activeGfsWrittenWords_[row];
                    activeGfsWrittenMask_[row].set(
                        static_cast<std::size_t>(rowOffset / 4U));
                    if (value != 0U) ++activeGfsWrittenNonZeroWords_[row];
                    ++activeGfsWriteCount_;
                }
            }
        }
        const auto clippedBegin = std::max(writeBegin, sourceBegin);
        const auto clippedEnd = std::min(writeEnd, sourceEnd);
        const auto firstChunk = static_cast<std::size_t>(
            (clippedBegin - sourceBegin) / kGfsSourceChunkSize);
        const auto lastChunk = static_cast<std::size_t>(
            (clippedEnd - 1U - sourceBegin) / kGfsSourceChunkSize);
        for (std::size_t chunkIndex = firstChunk;
             chunkIndex <= lastChunk && chunkIndex < gfsSourceChunks_.size();
             ++chunkIndex) {
            auto& chunk = gfsSourceChunks_[chunkIndex];
            if (chunk.writes == 0U) {
                chunk.firstStep = context_.step;
                chunk.firstAddress = address;
                chunk.firstValue = value;
                chunk.firstBlock = context_.blockAddress;
                chunk.firstCaller = context_.callerBlock;
            }
            ++chunk.writes;
            if (value != 0U) ++chunk.nonZeroWrites;
            chunk.lastStep = context_.step;
            chunk.lastAddress = address;
            chunk.lastValue = value;
            chunk.lastBlock = context_.blockAddress;
            chunk.lastCaller = context_.callerBlock;
        }
        if (writeEnd > static_cast<std::uint64_t>(kGfsSourceBoundaryBegin) &&
            writeBegin < static_cast<std::uint64_t>(kGfsSourceBoundaryEnd) &&
            gfsSourceBoundaryWrites_.size() < 1024U) {
            gfsSourceBoundaryWrites_.push_back(
                MemoryProbeRecord{address, width, value, true, context_});
        }
    }

    void observeGfsTaskConfigWrite(saturn::core::Address address,
                                   std::uint8_t width,
                                   std::uint32_t value) {
        const auto writeBegin = static_cast<std::uint64_t>(address);
        const auto writeEnd = writeBegin + width;
        if (writeEnd <= static_cast<std::uint64_t>(kGfsTaskConfigBegin) ||
            writeBegin >= static_cast<std::uint64_t>(kGfsTaskConfigEnd) ||
            gfsTaskConfigWrites_.size() >= 512U) {
            return;
        }
        gfsTaskConfigWrites_.push_back(
            MemoryProbeRecord{address, width, value, true, context_});
    }

    void observeGfsParserStackWrite(saturn::core::Address address,
                                    std::uint8_t width,
                                    std::uint32_t value) {
        if (context_.blockAddress != 0x0604029AU || width != 4U) return;
        const auto stackPointer = registerValue(saturn::sh2::Register::R15);
        if (address != stackPointer + 4U || gfsParserStackWrites_.size() >= 512U) return;
        gfsParserStackWrites_.push_back(
            MemoryProbeRecord{address, width, value, true, context_});
    }

    void observeGfsParserDataWrite(saturn::core::Address address,
                                   std::uint8_t width,
                                   std::uint32_t value) {
        const auto end = static_cast<std::uint64_t>(address) + width;
        const auto overlapsRange = [address, end](saturn::core::Address begin,
                                                   saturn::core::Address rangeEnd) {
            return end > static_cast<std::uint64_t>(begin) &&
                   static_cast<std::uint64_t>(address) < rangeEnd;
        };
        constexpr std::array<std::uint32_t, 5> kObjectOffsets{
            0x0CU, 0x14U, 0x64U, 0x6CU, 0x70U};
        bool objectField = false;
        for (const auto offset : kObjectOffsets) {
            if (overlapsRange(kGfsParserObjectAddress + offset,
                              kGfsParserObjectAddress + offset + 4U)) {
                objectField = true;
                break;
            }
        }
        const auto tableEntry = overlapsRange(
            kGfsParserTableEntryAddress, kGfsParserTableEntryAddress + 0x10U);
        if ((!objectField && !tableEntry) || gfsParserDataWrites_.size() >= 4096U) {
            return;
        }
        gfsParserDataWrites_.push_back(
            MemoryProbeRecord{address, width, value, true, context_});
    }

    void observeGfsParserSourceRead(saturn::core::Address address,
                                   std::uint8_t width,
                                   std::uint32_t value,
                                   std::uint32_t fifoOffsetBefore,
                                   std::uint32_t fifoOffsetAfter) {
        // 0x25818000 is the cache-bypassing alias of the CD Block FIFO
        // (0x05818000).  The table-copy loop reads four words from it.
        const auto isSource =
            (address >= kGfsParserSourceAddress &&
             address < kGfsParserSourceAddress + 0x10U) ||
            (address >= 0x05818000U && address < 0x05818010U);
        // Keep only the reads that feed the table entry under investigation;
        // the same FIFO is also used for the PVD and large streaming copies.
        const auto destination = registerValue(saturn::sh2::Register::R14);
        const auto feedsTargetTable =
            destination >= kGfsParserTableEntryAddress &&
            destination < kGfsParserTableEntryAddress + 0x10U;
        if (!isSource || !feedsTargetTable || gfsParserSourceReads_.size() >= 64U) return;
        const auto& cdStats = mmio_.cd().stats();
        gfsParserSourceReads_.push_back(GfsParserSourceReadRecord{
            MemoryProbeRecord{address, width, value, false, context_},
            fifoOffsetBefore,
            fifoOffsetAfter,
            cdStats.lastTransferLba,
            cdStats.lastTransferOffset,
            cdStats.lastTransferCount,
            cdStats.lastTransferPartition,
            cdStats.lastTransferInstruction,
        });
    }

    void observeCdFifoRead(saturn::core::Address address,
                           std::uint8_t width,
                           std::uint32_t value,
                           std::uint32_t fifoOffsetBefore,
                           std::uint32_t fifoOffsetAfter) {
        const auto physicalAddress = address & 0x1FFFFFFFU;
        const auto isByteFifo =
            physicalAddress >= saturn::platform::CdRuntime::kFifo &&
            physicalAddress < saturn::platform::CdRuntime::kFifo + 0x10U;
        const auto isWordFifo = width == 2U &&
            physicalAddress >= saturn::platform::CdRuntime::kWordFifo &&
            physicalAddress < saturn::platform::CdRuntime::kWordFifo + 0x10U;
        if ((!isByteFifo && !isWordFifo) ||
            context_.step < kCdFifoTraceStartStep ||
            fifoOffsetBefore % saturn::platform::CdRuntime::kLogicalSectorSize != 0U ||
            fifoOffsetAfter <= fifoOffsetBefore ||
            cdFifoReadRecords_.size() >= kCdFifoTraceLimit) {
            return;
        }

        const auto& cdStats = mmio_.cd().stats();
        if (cdStats.lastTransferPartition != 1U ||
            cdStats.lastTransferInstruction != 0x61U) {
            return;
        }
        constexpr std::array<saturn::sh2::Register, 16> kRegisters{
            saturn::sh2::Register::R0, saturn::sh2::Register::R1,
            saturn::sh2::Register::R2, saturn::sh2::Register::R3,
            saturn::sh2::Register::R4, saturn::sh2::Register::R5,
            saturn::sh2::Register::R6, saturn::sh2::Register::R7,
            saturn::sh2::Register::R8, saturn::sh2::Register::R9,
            saturn::sh2::Register::R10, saturn::sh2::Register::R11,
            saturn::sh2::Register::R12, saturn::sh2::Register::R13,
            saturn::sh2::Register::R14, saturn::sh2::Register::R15,
        };
        CdFifoReadRecord record{context_.step, context_.blockAddress,
                                context_.callerBlock, address, width, value,
                                fifoOffsetBefore, fifoOffsetAfter,
                                cdStats.lastTransferLba,
                                cdStats.lastTransferOffset,
                                cdStats.lastTransferCount,
                                cdStats.lastTransferPartition,
                                cdStats.lastTransferInstruction, {}};
        for (std::size_t index = 0; index < kRegisters.size(); ++index) {
            record.registers[index] = registerValue(kRegisters[index]);
        }
        cdFifoReadRecords_.push_back(std::move(record));
    }

    [[nodiscard]] static bool overlaps(saturn::core::Address address,
                                       std::uint8_t width,
                                       saturn::core::Address target) noexcept {
        const auto end = static_cast<std::uint64_t>(address) + width;
        return address <= target && static_cast<std::uint64_t>(target) < end;
    }

    [[nodiscard]] static bool overlapsTitleProgressState(
        saturn::core::Address address, std::uint8_t width) noexcept {
        const auto begin = static_cast<std::uint64_t>(address);
        const auto end = begin + width;
        constexpr auto kProgressBegin = std::uint64_t{0x06053350U};
        constexpr auto kProgressEnd = std::uint64_t{0x06053358U};
        return end > kProgressBegin && begin < kProgressEnd;
    }

    [[nodiscard]] std::uint32_t readRam(saturn::core::Address address,
                                        std::uint8_t width) const noexcept {
        std::uint32_t value{};
        for (std::uint8_t offset = 0; offset < width; ++offset) {
            const auto byte = state_.readRaw8(
                static_cast<saturn::core::Address>(address + offset));
            value = static_cast<std::uint32_t>((value << 8U) | byte);
        }
        return value;
    }

    [[nodiscard]] std::string readRamString(saturn::core::Address address,
                                            std::size_t maxLength) const {
        const auto highRam = address >= 0x06000000U && address < 0x06100000U;
        const auto lowRam = address >= 0x00200000U && address < 0x00300000U;
        if (!highRam && !lowRam) return {};
        const auto endAddress = highRam ? 0x06100000ULL : 0x00300000ULL;

        std::string value;
        value.reserve(maxLength);
        for (std::size_t offset = 0; offset < maxLength; ++offset) {
            const auto current = static_cast<std::uint64_t>(address) + offset;
            if (current >= endAddress) break;
            const auto byte = state_.readRaw8(static_cast<saturn::core::Address>(current));
            if (byte == 0U) break;
            if (byte < 0x20U || byte > 0x7EU) return {};
            value.push_back(static_cast<char>(byte));
        }
        return value;
    }

    [[nodiscard]] static std::optional<std::size_t> targetIndex(
        saturn::core::Address address) noexcept {
        const auto found = std::find(kTargetAddresses.begin(), kTargetAddresses.end(), address);
        if (found == kTargetAddresses.end()) return std::nullopt;
        return static_cast<std::size_t>(found - kTargetAddresses.begin());
    }

    [[nodiscard]] static bool isAsyncTarget(saturn::core::Address address) noexcept {
        return std::find(kAsyncTargetAddresses.begin(), kAsyncTargetAddresses.end(),
                         address) != kAsyncTargetAddresses.end();
    }

    [[nodiscard]] static bool isResolverFlowCheckpoint(
        saturn::core::Address address) noexcept {
        return std::find(kResolverFlowAddresses.begin(), kResolverFlowAddresses.end(),
                         address) != kResolverFlowAddresses.end();
    }

    void observeTitleGfsSetupBlock(saturn::core::Address blockAddress) {
        if (!titleQueueRequestSeen_) return;

        const auto handleArgument = registerValue(saturn::sh2::Register::R4);
        if (blockAddress == 0x06035DC2U) {
            titleGfsHandle_ = handleArgument;
        } else if (blockAddress == 0x06036BD0U && titleGfsHandle_ == 0U &&
                   handleArgument >= 12U) {
            titleGfsHandle_ = handleArgument - 12U;
        }

        const auto handleWordAt28 = titleGfsHandle_ == 0U
            ? 0U : readRam(titleGfsHandle_ + 0x28U, 4U);
        if (blockAddress == 0x06036BD0U) {
            titleGfsGateValue_ = handleWordAt28;
        }
        const auto isExtensionEntry = titleGfsGateValue_ != 0U &&
            blockAddress == 0x0603C006U && !titleGfsExtensionEntered_;
        constexpr std::array<saturn::core::Address, 21> kReadPathCheckpoints{
            0x06035E18U, 0x06035E20U, 0x06035E22U, 0x06035E24U,
            0x06035E2CU, 0x06035E30U, 0x06035E32U, 0x06035E34U,
            0x06035E36U, 0x06035E38U, 0x06035E3EU, 0x06035E50U,
            0x06035E5CU, 0x06035E60U, 0x06035E64U, 0x06035E6CU,
            0x06035E70U, 0x06035E78U, 0x06035E80U, 0x06035E88U,
            0x06035E8CU,
        };
        const auto isSetupCheckpoint = blockAddress == 0x06035DC2U ||
            blockAddress == 0x06035E10U || blockAddress == 0x06036BD0U ||
            std::find(kReadPathCheckpoints.begin(), kReadPathCheckpoints.end(),
                      blockAddress) != kReadPathCheckpoints.end();
        if (!isSetupCheckpoint && !isExtensionEntry) return;

        TitleGfsSetupRecord record{blockAddress, context_.callerBlock,
                                   context_.step, titleGfsHandle_,
                                   handleArgument,
                                   titleGfsHandle_ == 0U ? 0U :
                                       readRam(titleGfsHandle_ + 4U, 4U),
                                   handleWordAt28, {}, {}};
        if (titleGfsHandle_ != 0U) {
            record.ringFields = {
                readRam(titleGfsHandle_ + 0x5CU, 4U),
                readRam(titleGfsHandle_ + 0x60U, 4U),
                readRam(titleGfsHandle_ + 0x64U, 4U),
            };
        }
        constexpr std::array<saturn::sh2::Register, 16> kRegisters{
            saturn::sh2::Register::R0, saturn::sh2::Register::R1,
            saturn::sh2::Register::R2, saturn::sh2::Register::R3,
            saturn::sh2::Register::R4, saturn::sh2::Register::R5,
            saturn::sh2::Register::R6, saturn::sh2::Register::R7,
            saturn::sh2::Register::R8, saturn::sh2::Register::R9,
            saturn::sh2::Register::R10, saturn::sh2::Register::R11,
            saturn::sh2::Register::R12, saturn::sh2::Register::R13,
            saturn::sh2::Register::R14, saturn::sh2::Register::R15,
        };
        for (std::size_t index = 0; index < kRegisters.size(); ++index) {
            record.regs[index] = registerValue(kRegisters[index]);
        }
        if (titleGfsSetupRecords_.size() < 64U) {
            titleGfsSetupRecords_.push_back(std::move(record));
        }
        if (isExtensionEntry) titleGfsExtensionEntered_ = true;
    }

    [[nodiscard]] static bool isGfsProducerPathCheckpoint(
        saturn::core::Address block, saturn::core::Address caller) noexcept {
        if (block == 0x0603FC90U) return caller == 0x06040736U;
        if (block == 0x060401D4U) return caller == 0x0604075EU;
        return std::find(kGfsProducerPathAddresses.begin(),
                         kGfsProducerPathAddresses.end(), block) !=
               kGfsProducerPathAddresses.end();
    }

    [[nodiscard]] static std::optional<std::size_t> gfsCheckpointIndex(
        saturn::core::Address address) noexcept {
        const auto found = std::find(kGfsCheckpointAddresses.begin(),
                                     kGfsCheckpointAddresses.end(), address);
        if (found == kGfsCheckpointAddresses.end()) return std::nullopt;
        return static_cast<std::size_t>(found - kGfsCheckpointAddresses.begin());
    }

    [[nodiscard]] std::uint32_t registerValue(
        saturn::sh2::Register reg) const noexcept {
        const auto found = context_.state.find(saturn::sh2::stateSlot(reg));
        return found == context_.state.end() ? 0U : found->second;
    }

    // Focus on the PVD bytes and the small structure into which GFS copies
    // them; this avoids filling the probe with unrelated dispatcher state.
    static constexpr saturn::core::Address kProbeBegin = 0x0606A000U;
    static constexpr saturn::core::Address kProbeEnd = 0x0606A120U;
    // Begin just after the first sector's 0x62 deletion so the probe captures
    // the GFS validation pass instead of filling with CD bootstrap scratch.
    static constexpr std::size_t kProbeStartStep = 137'700U;
    static constexpr std::size_t kProbeLimit = 512U;
    static constexpr std::size_t kTargetTraceLimit = 512U;
    static constexpr std::size_t kGfsPathTraceLimit = 8192U;
    static constexpr std::size_t kGfsDecodeBoundaryTraceLimit = 4096U;
    static constexpr std::size_t kGfsSkipTraceLimit = 16384U;
    static constexpr std::size_t kGfsOutputCoverageLimit = 128U;
    static constexpr std::size_t kCdFifoTraceStartStep = 200'000'000U;
    static constexpr std::size_t kCdFifoTraceLimit = 8192U;
    static constexpr saturn::core::Address kWaitEventAddress = 0x060519CAU;
    static constexpr saturn::core::Address kTitleWaitFlagAddress = 0x0605E7F6U;
    static constexpr saturn::core::Address kTitleQueueIndexAddress = 0x0605E7F4U;
    static constexpr saturn::core::Address kTitleQueueCountAddress = 0x0605E7F5U;
    static constexpr saturn::core::Address kTitleQueueEntriesBegin = 0x0605E7B4U;
    static constexpr std::uint64_t kTitleQueueEntriesEnd = 0x0605E7F4U;
    static constexpr saturn::core::Address kCallbackAddress = 0x06034D72U;
    static constexpr std::array<saturn::core::Address, 33> kTargetAddresses{
        0x06015D48U, // foreground wait loop
        0x060303CCU, // SCU interrupt trampoline
        0x060303D8U, // application dispatcher
        0x06030854U, // dispatcher body
        kCallbackAddress, // callback expected to publish drawing/event work
        0x06039C22U, // asynchronous request entry
        0x0603A018U, // task state lookup entry
        0x0603A03CU, // resolved task pointer in R0
        0x0603A076U, // task state return path
        0x06015D30U, // request path before the wait
        0x06015D42U, // task state comparison
        0x06015D46U, // event clear before polling
        0x060166ACU, // SCU Timer 0 callback that publishes the wait event
        0x0603A646U, // active GFS task update method
        0x0603ED82U, // ring-buffer availability query
        0x0603EE10U, // ring-buffer producer advance
        0x0603EED6U, // ring-buffer consumer advance
        0x0603FC90U, // first retained-sector producer path
        0x060401D4U, // subsequent retained-sector producer path
        0x06040724U, // select the current producer descriptor
        0x0604073AU, // first producer path returned
        0x06040744U, // test the descriptor index against the count
        0x0604074CU, // select the subsequent producer descriptor
        0x06040762U, // subsequent producer path returned
        0x0601CE92U, // title async queue worker
        0x0601C5CEU, // queued resource resolver
        0x06035754U, // GFS resource-open entry
        0x0603AD4AU, // GFS resource-name table matcher
        0x0601D568U, // CAPCOM.ARC title-resource loader
        0x0601C5F0U, // queued resource resolver after GFS call
        0x06035770U, // GFS descriptor-not-ready error path
        0x06035790U, // GFS descriptor-ready path
        0x060357A2U, // GFS descriptor-operation error path
    };
    static constexpr std::array<saturn::core::Address, 11>
        kGfsProducerPathAddresses{
            0x06040724U, 0x0604073AU, 0x06040744U,
            0x0604074CU, 0x06040762U, 0x0604076CU,
            0x0604077CU, 0x06040784U, 0x06040794U,
            0x0604079CU, 0x060407A2U,
        };
    static constexpr std::array<saturn::core::Address, 8> kAsyncTargetAddresses{
        0x06039C22U, 0x0603A018U, 0x0603A03CU, 0x0603A076U,
        0x06015D30U, 0x06015D42U, 0x06015D46U, 0x0603A646U,
    };
    static constexpr std::array<saturn::core::Address, 39> kResolverFlowAddresses{
        0x0603AD4AU, 0x0603AD78U, 0x0603AD8CU, 0x0603AD8EU, 0x0603AD9AU,
        0x06035798U, 0x0603579AU, 0x0603579EU, 0x060357ACU,
        0x0601C5F0U, 0x0601C5F4U, 0x0601C604U, 0x0601C650U,
        0x0601C65CU, 0x0601C678U, 0x0601C67CU, 0x0601C680U,
        0x0601C68CU, 0x0601C6AAU,
        0x0601CE72U, 0x0601CE92U, 0x0601CE96U, 0x0601CE98U,
        0x0601CE9AU, 0x0601CE9EU, 0x0601CEAEU, 0x0601CEB4U,
        0x0601CEB6U, 0x0601CEBAU, 0x0601CED0U, 0x060101B6U,
        0x06035DC2U, 0x06035E10U, 0x06036BD0U,
        0x0027196AU, 0x0027197AU, 0x002719CEU, 0x002719E8U,
        0x0601CE2AU,
    };
    static constexpr std::array<saturn::core::Address, 13> kGfsCheckpointAddresses{
        0x0603A646U, // update entry
        0x0603A670U, // transfer state machine
        0x0603A68CU, // all requested units already queued
        0x0603A6ACU, // source availability query returned
        0x0603A6B4U, // at least one 0x800-byte sector is available
        0x0603A6FCU, // transfer size selected
        0x0603A71AU, // source read request returned
        0x0603A736U, // active-transfer check
        0x0603A740U, // poll active transfer
        0x0603A75CU, // active transfer completed successfully
        0x0603A78EU, // completion accounting
        0x0603A79EU, // mark transfer inactive
        0x0603A7A6U, // update exit
    };
    saturn::platform::BootRomServices& services_;
    saturn::platform::MmioInterceptor& mmio_;
    const saturn::sir::MachineState& state_;
    bool cdFifoOnly_{};
    saturn::sir::ExecutionContext context_;
    std::vector<MemoryProbeRecord> records_;
    std::vector<Vdp2MapWriteRecord> vdp2MapWrites_;
    std::vector<MemoryProbeRecord> eventRecords_;
    std::size_t eventRecordCount_{};
    std::vector<MemoryProbeRecord> titleWaitFlagWrites_;
    std::size_t titleWaitFlagWriteCount_{};
    std::vector<MemoryProbeRecord> titleQueueWrites_;
    std::size_t titleQueueWriteCount_{};
    std::vector<MemoryProbeRecord> titleProgressStateReads_;
    std::size_t titleProgressStateReadCount_{};
    std::vector<MemoryProbeRecord> titleProgressStateWrites_;
    std::size_t titleProgressStateWriteCount_{};
    std::vector<ResolverFlowRecord> resolverFlowRecords_;
    std::size_t resolverFlowRecordCount_{};
    std::vector<ResolverFlowRecord> stageArchiveFlowRecords_;
    std::size_t stageArchiveFlowRecordCount_{};
    std::unordered_map<saturn::core::Address, std::uint8_t>
        stageOverlayStates_;
    std::vector<TitleGfsSetupRecord> titleGfsSetupRecords_;
    std::vector<MemoryProbeRecord> titleGfsHookWrites_;
    std::size_t titleGfsHookWriteCount_{};
    std::vector<MemoryProbeRecord> titleGfsMemberWrites_;
    std::size_t titleGfsMemberWriteCount_{};
    saturn::core::Address titleGfsHandle_{};
    std::uint32_t titleGfsGateValue_{};
    bool titleGfsExtensionEntered_{};
    std::vector<MemoryProbeRecord> waitEventRecords_;
    std::size_t waitEventRecordCount_{};
    std::size_t waitEventReadCount_{};
    std::size_t waitEventWriteCount_{};
    std::size_t audioStatusCacheWriteCount_{};
    std::vector<MemoryProbeRecord> audioStatusCacheWrites_;
    std::vector<MemoryProbeRecord> waitEventWrites_;
    std::size_t waitEventWriteRecordCount_{};
    std::vector<MemoryProbeRecord> cdCommandRecords_;
    std::size_t cdCommandRecordCount_{};
    std::vector<CdPlayCommandRecord> cdPlayCommandRecords_;
    std::size_t cdPlayCommandRecordCount_{};
    std::vector<CdSectorRequestRecord> cdSectorRequestRecords_;
    std::size_t cdSectorRequestRecordCount_{};
    bool titleQueueRequestSeen_{};
    std::vector<BlockTransitionRecord> blockTransitions_;
    std::size_t blockTransitionCount_{};
    std::vector<BlockTransitionRecord> callbackTransitions_;
    std::size_t callbackTransitionCount_{};
    std::vector<BlockTransitionRecord> asyncTransitions_;
    std::size_t asyncTransitionCount_{};
    std::vector<DispatcherDispatchRecord> dispatcherDispatches_;
    std::vector<ObjectCallbackDispatchRecord> objectCallbackDispatches_;
    std::vector<ForegroundProbeRecord> foregroundProbeRecords_;
    std::vector<TitleCallbackStateRecord> titleCallbackStateRecords_;
    std::array<std::size_t, kTargetAddresses.size()> targetHits_{};
    std::array<std::size_t, kGfsCheckpointAddresses.size()> gfsCheckpointHits_{};
    std::array<std::optional<GfsUpdateRecord>, kGfsCheckpointAddresses.size()>
        lastGfsCheckpoint_{};
    std::vector<GfsProducerRecord> gfsProducerRecords_;
    std::optional<saturn::sir::ExecutionContext> lastFirstProducerExit_;
    std::array<std::uint32_t, 4> firstProducerExitStack_{};
    saturn::core::Address audioController_{};
    std::vector<MemoryProbeRecord> audioCursorWrites_;
    std::size_t audioCursorWriteCount_{};
    std::vector<SoundCommandRecord> soundCommands_;
    std::size_t soundCommandCount_{};
    std::size_t audioStatusPolls_{};
    std::size_t audioStatusDispatches_{};
    std::size_t audioConsumerAdvances_{};
    std::size_t audioStatusReadCount_{};
    std::optional<MemoryProbeRecord> lastAudioStatusRead_;
    std::size_t gfsProducerRecordCount_{};
    std::vector<GfsProducerPathRecord> gfsProducerPathRecords_;
    std::size_t gfsProducerPathRecordCount_{};
    std::vector<GfsTransferPollRecord> gfsTransferPolls_;
    std::size_t gfsTransferPollCount_{};
    std::vector<GfsAllocatorRecord> gfsAllocatorRecords_;
    std::size_t gfsAllocatorRecordCount_{};
    std::vector<GfsCapacityRecord> gfsCapacityRecords_;
    std::size_t gfsCapacityRecordCount_{};
    std::vector<DmaDescriptorRecord> dmaDescriptorRecords_;
    std::vector<GfsSourceChunkRecord> gfsSourceChunks_{
        kGfsSourceSize / kGfsSourceChunkSize + 1U};
    std::vector<MemoryProbeRecord> gfsSourceBoundaryWrites_;
    std::vector<MemoryProbeRecord> gfsTaskConfigWrites_;
    std::vector<MemoryProbeRecord> gfsParserStackWrites_;
    std::vector<MemoryProbeRecord> gfsParserDataWrites_;
    std::vector<GfsParserSourceReadRecord> gfsParserSourceReads_;
    std::vector<CdFifoReadRecord> cdFifoReadRecords_;
    std::size_t gfsSourceWriteCount_{};
    std::size_t gfsSourceNonZeroWriteCount_{};
    std::vector<GfsDecodeRecord> gfsDecodeRecords_;
    std::vector<GfsDecodeRecord> gfsDecodeBoundaryRecords_;
    std::vector<GfsDecodeRecord> gfsParserCalculationRecords_;
    std::vector<GfsOutputCoverageRecord> gfsOutputCoverageRecords_;
    std::vector<GfsSkipRunRecord> gfsSkipRecords_;
    saturn::core::Address activeGfsOutputBase_{};
    saturn::core::Address activeGfsCompressedSource_{};
    std::uint32_t activeGfsOutputStride_{};
    std::uint32_t activeGfsOutputWidth_{};
    std::uint32_t activeGfsOutputHeight_{};
    bool activeGfsOutputCoverage_{};
    std::array<std::uint16_t, 224> activeGfsWrittenWords_{};
    std::array<std::uint16_t, 224> activeGfsWrittenNonZeroWords_{};
    std::array<std::bitset<0x140U>, 224> activeGfsWrittenMask_{};
    std::vector<std::uint32_t> activeGfsPreOutput_;
    std::uint64_t activeGfsPreFrameHash_{};
    std::uint32_t activeGfsWriteCount_{};
    std::uint32_t activeGfsMaxWrittenRow_{};
    std::uint64_t activeGfsMaxWrittenOffset_{};
    saturn::core::Address activeGfsFirstWrite_{};
    saturn::core::Address activeGfsLastWrite_{};
    saturn::core::Address resolvedTaskAddress_{};
};

// A low-overhead, address-filtered observer used with frame captures. It
// records writes routed through MachineState, including CPU stores and the
// SH-2, SCU, and CD transfers implemented through that interface, without
// enabling the much broader launch trace probe.
class SpriteSourceWriteProbe final : public saturn::sir::MemoryInterceptor {
public:
    SpriteSourceWriteProbe(saturn::sir::MemoryInterceptor& downstream,
                           const saturn::sir::MachineState& state)
        : downstream_(downstream), state_(state) {}

    void setExecutionContext(const saturn::sir::ExecutionContext& context) override {
        step_ = context.step;
        block_ = context.blockAddress;
        caller_ = context.callerBlock;
        downstream_.setExecutionContext(context);
    }

    [[nodiscard]] saturn::core::Address backingAddress(
        saturn::core::Address address) const noexcept override {
        return downstream_.backingAddress(address);
    }

    [[nodiscard]] std::optional<std::uint32_t> read(
        saturn::core::Address address, std::uint8_t width) override {
        return downstream_.read(address, width);
    }

    [[nodiscard]] bool write(saturn::core::Address address, std::uint8_t width,
                             std::uint32_t value) override {
        const auto physical = address & 0x1FFFFFFFU;
        const auto writeEnd = static_cast<std::uint64_t>(physical) + width;
        constexpr std::uint64_t kSourceBegin = 0x06003000U;
        // The observed SCU upload copies 0x780 bytes from 0x06003000. Keep
        // the full transfer range so the trace includes all 15 4-bpp tiles.
        constexpr std::uint64_t kSourceEnd = 0x06003780U;
        constexpr std::uint64_t kOffsetBegin = 0x06068598U;
        constexpr std::uint64_t kOffsetEnd = kOffsetBegin + sizeof(std::uint32_t);
        const auto begin = static_cast<std::uint64_t>(physical);
        const auto sourceWrite = writeEnd > kSourceBegin && begin < kSourceEnd;
        const auto pointerWrite = writeEnd > kOffsetBegin && begin < kOffsetEnd;
        if (sourceWrite || pointerWrite) {
            const auto previousValue = readRaw(address, width);
            ++writeCount_;
            if (sourceWrite) ++sourceWriteCount_;
            if (pointerWrite) ++pointerWriteCount_;
            // The loader rewrites the unchanged zero offset on every frame.
            // Keep those stores in the summary count, but do not let them evict
            // the source-buffer writes that can explain a missing sprite.
            if (sourceWrite || previousValue != value) {
                SpriteSourceWriteRecord record{};
                record.step = step_;
                record.block = block_;
                record.caller = caller_;
                record.address = address;
                record.physicalAddress = physical;
                record.width = width;
                record.previousValue = previousValue;
                record.value = value;
                record.pointerWrite = pointerWrite;
                if (writes_.size() < kWriteLimit) {
                    writes_.push_back(std::move(record));
                } else {
                    writes_[recordedWriteCount_ % kWriteLimit] = std::move(record);
                }
                ++recordedWriteCount_;
            }
        }
        return downstream_.write(address, width, value);
    }

    [[nodiscard]] const std::vector<SpriteSourceWriteRecord>& writes() const noexcept {
        return writes_;
    }
    [[nodiscard]] std::size_t writeCount() const noexcept { return writeCount_; }
    [[nodiscard]] std::size_t sourceWriteCount() const noexcept {
        return sourceWriteCount_;
    }
    [[nodiscard]] std::size_t pointerWriteCount() const noexcept {
        return pointerWriteCount_;
    }
    [[nodiscard]] std::size_t recordedWriteCount() const noexcept {
        return recordedWriteCount_;
    }

private:
    [[nodiscard]] std::uint32_t readRaw(saturn::core::Address address,
                                        std::uint8_t width) const noexcept {
        std::uint32_t value{};
        for (std::uint8_t offset = 0; offset < width; ++offset) {
            value = static_cast<std::uint32_t>(
                (value << 8U) | state_.readRaw8(address + offset));
        }
        return value;
    }

    static constexpr std::size_t kWriteLimit = 8192U;
    saturn::sir::MemoryInterceptor& downstream_;
    const saturn::sir::MachineState& state_;
    std::size_t step_{};
    saturn::core::Address block_{};
    saturn::core::Address caller_{};
    std::vector<SpriteSourceWriteRecord> writes_;
    std::size_t writeCount_{};
    std::size_t sourceWriteCount_{};
    std::size_t pointerWriteCount_{};
    std::size_t recordedWriteCount_{};
};

void usage(std::ostream& stream) {
    stream << "Usage:\n"
           << "  saturn-sh2 decode <binary> --base <address>\n"
           << "  saturn-sh2 cfg <binary> --base <address> --entry <address> [--output <cfg.dot>]\n"
           << "  saturn-sh2 analyze <binary> --base <address> --entry <address>\n"
           << "  saturn-sh2 run <binary> --base <address> --entry <address> [--steps <count>] [--interpreter]\n"
           << "  saturn-sh2 boot|launch <aip.bin> --aip-base <address> --program <0.BIN>\n"
           << "      --program-base <address> --expected-entry <address> [--ip <IP.BIN>\n"
           << "      --ip-base <address>] [--disc <image> [--disc-sector-size <bytes>]]\n"
           << "      [--resource <iso-path> --resource-base <address>]\n"
           << "      [--frame-dir <directory>] [--frame-changes-only]\n"
           << "      [--black-frame-probe <directory>]\n"
           << "      [--vdp2-map-trace <directory>]\n"
           << "      [--vdp2-map-read-trace <directory>]\n"
           << "      [--audio-output <wav>]\n"
           << "      [--scsp-driver] (real 68000 and SCSP slots; requires Musashi build)\n"
           << "      [--aot-entry-output <addresses.txt>] (record resolved SH-2 targets)\n"
           << "      [--aot-module-output <module.cpp>] (emit all SIR blocks discovered in this run)\n"
           << "      [--control-trace <events.csv>] (record scripted pad, SMPC and CD events)\n"
           << "      [--x4-player-samples <events.csv>] (sample X before each scripted pad change)\n"
           << "      [--debug-session <directory>] (F8 or capture.request saves a live diagnostic clip)\n"
           << "      [--control-trace-from-step <count>] (requires --control-trace)\n"
           << "      [--control-trace-oreg2-only] (retain only OREG2 SMPC reads plus pad/CD events)\n"
           << "      [--window] [--audio-live] [--renderer auto|cpu|vulkan]\n"
           << "      [--pad1 <buttons>] [--pad2 <buttons>] [--pad-after-blocks <count>]\n"
           << "      [--pad-sequence <step:buttons;...>] (use release to clear)\n"
           << "      [--pad-live-after-blocks <count>] (give port 1 to keyboard/XInput)\n"
           << "      [--pad-release-after-blocks <count>]\n"
           << "      [--pad-second-press-after-blocks <count>]\n"
           << "      [--pad-second-release-after-blocks <count>]\n"
           << "      [--pad-third-press-after-blocks <count>]\n"
           << "      [--pad-third-release-after-blocks <count>]\n"
           << "      [--frame-capture-after-blocks <count>] (requires --frame-dir)\n"
           << "      [--trace-cd-sectors] (requires --frame-dir; write CD PLAY/sector/FIFO CSVs)\n"
           << "      [--steps <count>] [--vblank-blocks <count>] [--trace]\n"
           << "      [--trace-game]\n"
           << "      [--trace-foreground]\n"
           << "      [--continue-after-execdmp] [--interpreter]\n";
}

std::string formatWord(std::uint16_t word) {
    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex << std::setfill('0')
           << std::setw(4) << word;
    return output.str();
}

std::optional<saturn::core::Address> parseAddress(std::string_view text) {
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
        base = 16;
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint32_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<Arguments> parseArguments(int argc, char** argv) {
    if (argc < 5) {
        return std::nullopt;
    }
    Arguments arguments{};
    arguments.command = argv[1];
    arguments.input = argv[2];
    for (int index = 3; index < argc; ++index) {
        const std::string_view option = argv[index];
        if ((option == "--base" || option == "--aip-base" || option == "--ip" ||
             option == "--ip-base" || option == "--program" ||
             option == "--program-base" || option == "--entry" ||
             option == "--expected-entry" || option == "--output" ||
             option == "--disc" || option == "--disc-sector-size" ||
             option == "--resource" || option == "--resource-base" ||
             option == "--frame-dir" || option == "--black-frame-probe" ||
             option == "--vdp2-map-trace" ||
             option == "--vdp2-map-read-trace" ||
             option == "--audio-output" || option == "--renderer" ||
             option == "--aot-entry-output" ||
             option == "--aot-module-output" ||
              option == "--scsp-requests" ||
              option == "--control-trace" ||
              option == "--x4-player-samples" ||
              option == "--control-trace-from-step" ||
             option == "--pad-sequence" ||
             option == "--poke" ||
             option == "--watch-memory" ||
             option == "--watch-dir" ||
             option == "--watch-from" ||
             option == "--pad-live-after-blocks" ||
             option == "--steps" || option == "--vblank-blocks" ||
             option == "--pad1" || option == "--pad2" || option == "--pad-after-blocks" ||
             option == "--pad-release-after-blocks" ||
             option == "--pad-second-press-after-blocks" ||
             option == "--pad-second-release-after-blocks" ||
             option == "--pad-third-press-after-blocks" ||
             option == "--pad-third-release-after-blocks" ||
             option == "--frame-capture-after-blocks" || option == "--debug-session") &&
            index + 1 >= argc) {
            return std::nullopt;
        }
        if (option == "--base") {
            const auto value = parseAddress(argv[++index]);
            if (!value) return std::nullopt;
            arguments.base = *value;
            arguments.hasBase = true;
        } else if (option == "--aip-base") {
            const auto value = parseAddress(argv[++index]);
            if (!value) return std::nullopt;
            arguments.aipBase = *value;
            arguments.hasAipBase = true;
        } else if (option == "--ip") {
            arguments.ipImage = std::filesystem::path(argv[++index]);
        } else if (option == "--ip-base") {
            const auto value = parseAddress(argv[++index]);
            if (!value) return std::nullopt;
            arguments.ipBase = *value;
            arguments.hasIpBase = true;
        } else if (option == "--program") {
            arguments.program = std::filesystem::path(argv[++index]);
        } else if (option == "--disc") {
            arguments.discImage = std::filesystem::path(argv[++index]);
        } else if (option == "--resource") {
            arguments.resourcePath = std::filesystem::path(argv[++index]);
        } else if (option == "--resource-base") {
            const auto value = parseAddress(argv[++index]);
            if (!value) return std::nullopt;
            arguments.resourceBase = *value;
        } else if (option == "--frame-dir") {
            arguments.frameDirectory = std::filesystem::path(argv[++index]);
        } else if (option == "--debug-session") {
            arguments.debugSessionDirectory = std::filesystem::path(argv[++index]);
        } else if (option == "--black-frame-probe") {
            arguments.blackFrameProbe = std::filesystem::path(argv[++index]);
        } else if (option == "--vdp2-map-trace") {
            arguments.vdp2MapTrace = std::filesystem::path(argv[++index]);
        } else if (option == "--vdp2-map-read-trace") {
            arguments.vdp2MapReadTrace = std::filesystem::path(argv[++index]);
        } else if (option == "--renderer") {
            arguments.renderer = argv[++index];
            if (arguments.renderer != "auto" && arguments.renderer != "cpu" &&
                arguments.renderer != "vulkan") return std::nullopt;
            arguments.rendererSpecified = true;
        } else if (option == "--audio-output") {
            arguments.audioOutput = std::filesystem::path(argv[++index]);
        } else if (option == "--scsp-requests") {
            arguments.scspRequestsOutput = std::filesystem::path(argv[++index]);
        } else if (option == "--aot-entry-output") {
            arguments.aotEntryOutput = std::filesystem::path(argv[++index]);
        } else if (option == "--aot-module-output") {
            arguments.aotModuleOutput = std::filesystem::path(argv[++index]);
        } else if (option == "--control-trace") {
            arguments.controlTraceOutput = std::filesystem::path(argv[++index]);
        } else if (option == "--x4-player-samples") {
            arguments.x4PlayerSamplesOutput = std::filesystem::path(argv[++index]);
        } else if (option == "--control-trace-from-step") {
            std::size_t value{};
            const std::string_view text = argv[++index];
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
            arguments.controlTraceFromStep = value;
        } else if (option == "--control-trace-oreg2-only") {
            arguments.controlTraceOreg2Only = true;
        } else if (option == "--pad-sequence") {
            arguments.padSequence = argv[++index];
        } else if (option == "--poke") {
            // STEP:ADDR:VALUE[:WIDTH], all numbers in decimal or 0x form.
            const std::string_view text = argv[++index];
            const auto split = [](std::string_view value, char separator) {
                std::vector<std::string> parts;
                std::size_t start = 0;
                while (true) {
                    const auto position = value.find(separator, start);
                    parts.emplace_back(value.substr(start, position == std::string_view::npos
                                                               ? std::string_view::npos
                                                               : position - start));
                    if (position == std::string_view::npos) break;
                    start = position + 1;
                }
                return parts;
            };
            const auto parts = split(text, ':');
            if (parts.size() < 3 || parts.size() > 4) return std::nullopt;
            const auto number = [](const std::string& value) -> std::optional<std::uint64_t> {
                try {
                    return std::stoull(value, nullptr, 0);
                } catch (...) {
                    return std::nullopt;
                }
            };
            const auto step = number(parts[0]);
            const auto address = number(parts[1]);
            const auto value = number(parts[2]);
            const auto width = parts.size() == 4 ? number(parts[3]) : std::optional<std::uint64_t>{1};
            if (!step || !address || !value || !width) return std::nullopt;
            if (*address > 0xFFFFFFFFULL || *value > 0xFFFFFFFFULL ||
                (*width != 1 && *width != 2 && *width != 4)) {
                return std::nullopt;
            }
            arguments.pokes.emplace_back(static_cast<std::size_t>(*step),
                                         static_cast<saturn::core::Address>(*address),
                                         static_cast<std::uint32_t>(*value),
                                         static_cast<std::uint8_t>(*width));
        } else if (option == "--watch-memory") {
            // ADDR:LENGTH:PERIOD, sampled once when the step counter crosses
            // each multiple of PERIOD starting at --watch-from.
            const std::string text = argv[++index];
            const auto first = text.find(':');
            const auto second = text.find(':', first + 1);
            if (first == std::string::npos || second == std::string::npos) return std::nullopt;
            try {
                arguments.watchAddress = static_cast<saturn::core::Address>(
                    std::stoull(text.substr(0, first), nullptr, 0));
                arguments.watchLength = static_cast<std::size_t>(
                    std::stoull(text.substr(first + 1, second - first - 1), nullptr, 0));
                arguments.watchPeriod = static_cast<std::size_t>(
                    std::stoull(text.substr(second + 1), nullptr, 0));
            } catch (...) {
                return std::nullopt;
            }
            if (arguments.watchLength == 0 || arguments.watchLength > 0x100000 ||
                arguments.watchPeriod == 0) {
                return std::nullopt;
            }
        } else if (option == "--watch-dir") {
            arguments.watchDirectory = std::filesystem::path(argv[++index]);
        } else if (option == "--watch-from") {
            try {
                arguments.watchFrom = static_cast<std::size_t>(std::stoull(argv[++index], nullptr, 0));
            } catch (...) {
                return std::nullopt;
            }
        } else if (option == "--pad-live-after-blocks") {
            std::size_t value{};
            const std::string_view text = argv[++index];
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
            arguments.padLiveAfterBlocks = value;
        } else if (option == "--frame-changes-only") {
            arguments.frameChangesOnly = true;
        } else if (option == "--disc-sector-size") {
            std::size_t value{};
            const std::string_view text = argv[++index];
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() || value == 0U) {
                return std::nullopt;
            }
            arguments.discSectorSize = value;
        } else if (option == "--program-base") {
            const auto value = parseAddress(argv[++index]);
            if (!value) return std::nullopt;
            arguments.programBase = *value;
            arguments.hasProgramBase = true;
        } else if (option == "--entry") {
            const auto value = parseAddress(argv[++index]);
            if (!value) return std::nullopt;
            arguments.entry = *value;
        } else if (option == "--expected-entry") {
            const auto value = parseAddress(argv[++index]);
            if (!value) return std::nullopt;
            arguments.expectedEntry = *value;
        } else if (option == "--output") {
            arguments.output = std::filesystem::path(argv[++index]);
        } else if (option == "--steps") {
            std::size_t value{};
            const std::string_view text = argv[++index];
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
            arguments.maxSteps = value;
            arguments.stepsSpecified = true;
        } else if (option == "--vblank-blocks") {
            std::size_t value{};
            const std::string_view text = argv[++index];
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
            arguments.vblankPeriodBlocks = value;
            arguments.vblankSpecified = true;
        } else if (option == "--pad1" || option == "--pad2") {
            const auto port = option == "--pad1" ? 0U : 1U;
            arguments.padButtons[port] = argv[++index];
        } else if (option == "--pad-after-blocks") {
            const std::string_view text = argv[++index];
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(),
                                                    arguments.padAfterBlocks);
            if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
        } else if (option == "--pad-release-after-blocks" ||
                   option == "--pad-second-press-after-blocks" ||
                   option == "--pad-second-release-after-blocks" ||
                   option == "--pad-third-press-after-blocks" ||
                   option == "--pad-third-release-after-blocks" ||
                   option == "--frame-capture-after-blocks") {
            std::size_t value{};
            const std::string_view text = argv[++index];
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
            if (option == "--pad-release-after-blocks") {
                arguments.padReleaseAfterBlocks = value;
            } else if (option == "--pad-second-press-after-blocks") {
                arguments.padSecondPressAfterBlocks = value;
            } else if (option == "--pad-second-release-after-blocks") {
                arguments.padSecondReleaseAfterBlocks = value;
            } else if (option == "--pad-third-press-after-blocks") {
                arguments.padThirdPressAfterBlocks = value;
            } else if (option == "--pad-third-release-after-blocks") {
                arguments.padThirdReleaseAfterBlocks = value;
            } else {
                arguments.frameCaptureAfterBlocks = value;
            }
        } else if (option == "--trace") {
            arguments.trace = true;
        } else if (option == "--trace-cd-sectors") {
            arguments.traceCdSectors = true;
        } else if (option == "--interpreter") {
            arguments.forceInterpreter = true;
        } else if (option == "--trace-game") {
            arguments.traceGame = true;
        } else if (option == "--trace-foreground") {
            arguments.traceForeground = true;
        } else if (option == "--window") {
            arguments.window = true;
        } else if (option == "--audio-live") {
            arguments.audioLive = true;
        } else if (option == "--scsp-driver") {
            arguments.scspDriver = true;
        } else if (option == "--continue-after-execdmp") {
            arguments.continueAfterExecuteDmp = true;
        } else {
            return std::nullopt;
        }
    }
    if (arguments.command == "launch") {
        if (!arguments.rendererSpecified) arguments.renderer = "auto";
        arguments.window = true;
        arguments.audioLive = true;
        // A launcher session is interactive by default. Keep explicit
        // --steps/--vblank-blocks values intact for deterministic smoke tests.
        if (!arguments.stepsSpecified) {
            arguments.maxSteps = std::numeric_limits<std::size_t>::max();
        }
        // Give the original CPK decoder enough work between display/audio
        // boundaries to refill PCM before it wraps. A smaller budget can
        // display 60 FPS while replaying stale samples from the sound ring.
        if (!arguments.vblankSpecified) arguments.vblankPeriodBlocks = 14'000U;
        arguments.continueAfterExecuteDmp = true;
    }
    if ((arguments.aotEntryOutput || arguments.aotModuleOutput) &&
        arguments.command != "boot" && arguments.command != "launch") {
        return std::nullopt;
    }
    if (arguments.controlTraceOutput &&
        arguments.command != "boot" && arguments.command != "launch") {
        return std::nullopt;
    }
    if (arguments.controlTraceFromStep && !arguments.controlTraceOutput) {
        return std::nullopt;
    }
    if (arguments.controlTraceOreg2Only && !arguments.controlTraceOutput) {
        return std::nullopt;
    }
    if (arguments.x4PlayerSamplesOutput &&
        ((arguments.command != "boot" && arguments.command != "launch") ||
         !arguments.padSequence)) {
        return std::nullopt;
    }
    if (arguments.frameCaptureAfterBlocks && !arguments.frameDirectory) return std::nullopt;
    if (arguments.debugSessionDirectory &&
        (arguments.command != "boot" && arguments.command != "launch")) return std::nullopt;
    if (arguments.scspDriver && arguments.command != "boot" && arguments.command != "launch")
        return std::nullopt;
    if (arguments.traceCdSectors && !arguments.frameDirectory) return std::nullopt;
    if (arguments.padLiveAfterBlocks &&
        (!arguments.padSequence || !arguments.window || arguments.padButtons[0] ||
         arguments.padReleaseAfterBlocks || arguments.padSecondPressAfterBlocks ||
         arguments.padSecondReleaseAfterBlocks || arguments.padThirdPressAfterBlocks ||
         arguments.padThirdReleaseAfterBlocks)) return std::nullopt;
    return arguments;
}

std::optional<saturn::platform::DigitalPadState> parsePadState(std::string_view text) {
    saturn::platform::DigitalPadState state;
    if (text == "release" || text == "neutral") {
        return state;
    }
    if (text == "none" || text == "disconnected") {
        state.connected = false;
        return state;
    }
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto end = text.find(',', begin);
        const auto name = text.substr(begin, end == std::string_view::npos
                                                ? text.size() - begin
                                                : end - begin);
        if (!name.empty()) {
            const auto button = saturn::platform::parsePadButton(name);
            if (!button) {
                return std::nullopt;
            }
            state.set(*button);
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    return state;
}

std::optional<std::vector<std::pair<std::size_t, saturn::platform::DigitalPadState>>>
parsePadSequence(std::string_view text) {
    std::vector<std::pair<std::size_t, saturn::platform::DigitalPadState>> events;
    std::size_t begin{};
    std::size_t previousStep{};
    bool hasPreviousStep{};
    while (begin < text.size()) {
        const auto end = text.find(';', begin);
        const auto eventText = text.substr(begin, end == std::string_view::npos
                                                       ? text.size() - begin
                                                       : end - begin);
        const auto separator = eventText.find(':');
        if (separator == std::string_view::npos || separator == 0U ||
            separator + 1U >= eventText.size()) {
            return std::nullopt;
        }
        std::size_t step{};
        const auto stepText = eventText.substr(0U, separator);
        const auto [stepEnd, stepError] = std::from_chars(
            stepText.data(), stepText.data() + stepText.size(), step);
        if (stepError != std::errc{} || stepEnd != stepText.data() + stepText.size() ||
            (hasPreviousStep && step < previousStep)) {
            return std::nullopt;
        }
        const auto state = parsePadState(eventText.substr(separator + 1U));
        if (!state) return std::nullopt;
        events.emplace_back(step, *state);
        previousStep = step;
        hasPreviousStep = true;
        if (end == std::string_view::npos) break;
        begin = end + 1U;
    }
    if (events.empty()) return std::nullopt;
    return events;
}

std::uint32_t contextRegister(const saturn::sir::ExecutionContext& context,
                              saturn::sh2::Register reg) {
    const auto found = context.state.find(saturn::sh2::stateSlot(reg));
    return found == context.state.end() ? 0U : found->second;
}

void printContext(const saturn::sir::ExecutionContext& context) {
    std::cout << " block=" << saturn::sh2::formatAddress(context.blockAddress)
              << " caller=" << saturn::sh2::formatAddress(context.callerBlock)
              << " step=" << context.step
              << " r0=" << saturn::sh2::formatAddress(
                     contextRegister(context, saturn::sh2::Register::R0))
              << " r1=" << saturn::sh2::formatAddress(
                     contextRegister(context, saturn::sh2::Register::R1))
              << " r2=" << saturn::sh2::formatAddress(
                     contextRegister(context, saturn::sh2::Register::R2))
              << " r3=" << saturn::sh2::formatAddress(
                     contextRegister(context, saturn::sh2::Register::R3))
              << " r4=" << saturn::sh2::formatAddress(
                     contextRegister(context, saturn::sh2::Register::R4))
              << " r5=" << saturn::sh2::formatAddress(
                     contextRegister(context, saturn::sh2::Register::R5))
              << " r6=" << saturn::sh2::formatAddress(
                     contextRegister(context, saturn::sh2::Register::R6))
              << " r7=" << saturn::sh2::formatAddress(
                     contextRegister(context, saturn::sh2::Register::R7))
              << " r13=" << saturn::sh2::formatAddress(
                     contextRegister(context, saturn::sh2::Register::R13))
              << " r14=" << saturn::sh2::formatAddress(
                     contextRegister(context, saturn::sh2::Register::R14))
              << " sp=" << saturn::sh2::formatAddress(
                     contextRegister(context, saturn::sh2::Register::R15))
              << " pr=" << saturn::sh2::formatAddress(
                     contextRegister(context, saturn::sh2::Register::PR));
}

int decode(const saturn::core::MemoryMap& image) {
    const saturn::sh2::Decoder decoder;
    for (const auto& [_, segment] : image.segments()) {
        if (segment.bytes.size() % 2 != 0) {
            std::cerr << "warning: trailing byte ignored in segment at "
                      << saturn::sh2::formatAddress(segment.base)
                      << "; SH-2 instructions are 16-bit words\n";
        }
        for (std::size_t offset = 0; offset + 1 < segment.bytes.size(); offset += 2) {
            const auto address = static_cast<saturn::core::Address>(segment.base + offset);
            const auto word = image.read16BE(address);
            const auto instruction = decoder.decode(address, *word).instruction;
            std::cout << saturn::sh2::formatAddress(address) << "  " << std::hex << std::uppercase
                      << *word << std::dec << "  " << saturn::sh2::formatInstruction(instruction);
            if (!instruction.valid) {
                std::cout << "  ; unsupported";
            } else if (instruction.flow != saturn::sh2::FlowKind::Fallthrough) {
                std::cout << "  ; flow=" << saturn::sh2::flowName(instruction.flow)
                          << ", delay=" << saturn::sh2::delaySlotName(instruction.delaySlot);
            }
            std::cout << '\n';
        }
    }
    return 0;
}

int cfg(const saturn::core::MemoryMap& image, saturn::core::Address entry,
        const std::optional<std::filesystem::path>& outputPath) {
    const auto graph = saturn::sh2::BlockBuilder{}.build(image, std::vector{entry});
    const auto dot = saturn::sh2::toDot(graph);
    if (!outputPath) {
        std::cout << dot;
        return 0;
    }
    std::ofstream output(*outputPath, std::ios::binary);
    if (!output) {
        std::cerr << "error: unable to write " << outputPath->string() << '\n';
        return 1;
    }
    output << dot;
    std::cout << "wrote " << outputPath->string() << " (" << graph.blocks.size()
              << " basic blocks)\n";
    for (const auto& [address, block] : graph.blocks) {
        for (const auto& edge : block.successors) {
            if (edge.kind == saturn::sh2::EdgeKind::Indirect && edge.target &&
                !image.contains(*edge.target)) {
                std::cout << "resolved indirect target "
                          << saturn::sh2::formatAddress(address) << " -> "
                          << saturn::sh2::formatAddress(*edge.target)
                          << " (outside image)\n";
            }
        }
    }
    return output ? 0 : 1;
}

int analyze(const saturn::core::MemoryMap& image, saturn::core::Address entry) {
    const auto graph = saturn::sh2::BlockBuilder{}.build(image, std::vector{entry});
    const auto report = saturn::sh2::analyzeCFG(graph);
    saturn::sir::Module module;
    const auto moduleReport = saturn::sh2::lowerCFG(graph, module);
    const auto lifterCoverage = report.fallthroughInstructionCount == 0
        ? 0.0
        : 100.0 * static_cast<double>(report.liftableInstructionCount) /
              static_cast<double>(report.fallthroughInstructionCount);

    std::cout << "SH-2 reachable-code analysis\n"
              << "  entry: " << saturn::sh2::formatAddress(entry) << '\n'
              << "  basic blocks: " << report.basicBlockCount << '\n'
              << "  decoded instructions: " << report.instructionCount << '\n'
              << "  SIR-liftable non-control instructions: "
              << report.liftableInstructionCount << '/' << report.fallthroughInstructionCount
              << " (" << std::fixed << std::setprecision(1) << lifterCoverage << "%)\n"
              << "  concrete SIR blocks: " << moduleReport.loweredBlocks << '/'
              << graph.blocks.size() << '\n'
              << "  concrete SIR operations: " << module.instructionCount() << '\n'
              << "  unsupported sites: " << report.unsupportedSites.size() << '\n'
              << "  interrupted blocks: " << report.interruptedBlockCount << '\n'
              << "  unsupported delay slots: " << report.invalidDelaySlotCount << '\n'
              << "  missing delay slots: " << report.missingDelaySlotCount << '\n'
              << "  control transfers in delay slots: "
              << report.controlTransferDelaySlotCount << '\n'
              << "  unresolved indirect transfers: "
              << report.unresolvedIndirectTransfers.size() << "\n\n";

    std::cout << "Block lowering failures:\n";
    if (moduleReport.failures.empty()) {
        std::cout << "  none\n";
    }
    for (const auto& failure : moduleReport.failures) {
        std::cout << "  " << saturn::sh2::formatAddress(failure.address)
                  << " status=" << saturn::sh2::blockLiftStatusName(failure.status) << '\n';
    }

    std::cout << "Unsupported opcode priority:\n";
    if (report.unsupportedOpcodes.empty()) {
        std::cout << "  none\n";
    }
    for (const auto& opcode : report.unsupportedOpcodes) {
        std::cout << "  [" << saturn::sh2::priorityName(opcode.priority) << "] "
                  << formatWord(opcode.raw) << " count=" << opcode.count
                  << " addresses=";
        for (std::size_t index = 0; index < opcode.addresses.size(); ++index) {
            if (index != 0) {
                std::cout << ',';
            }
            std::cout << saturn::sh2::formatAddress(opcode.addresses[index]);
        }
        std::cout << '\n';
    }

    std::cout << "\nMissing SIR lifters:\n";
    if (report.unliftedOpcodes.empty()) {
        std::cout << "  none\n";
    }
    for (const auto& opcode : report.unliftedOpcodes) {
        std::cout << "  " << saturn::sh2::opcodeName(opcode.opcode)
                  << " count=" << opcode.count << " sample=";
        const auto sampleCount = std::min<std::size_t>(opcode.addresses.size(), 4);
        for (std::size_t index = 0; index < sampleCount; ++index) {
            if (index != 0) {
                std::cout << ',';
            }
            std::cout << saturn::sh2::formatAddress(opcode.addresses[index]);
        }
        std::cout << '\n';
    }

    std::cout << "\nInterrupted/unsafe sites:\n";
    if (report.unsupportedSites.empty()) {
        std::cout << "  none\n";
    }
    for (const auto& site : report.unsupportedSites) {
        std::cout << "  " << saturn::sh2::formatAddress(site.address)
                  << " word=" << formatWord(site.raw)
                  << " block=" << saturn::sh2::formatAddress(site.block)
                  << " context=" << saturn::sh2::unsupportedContextName(site.context)
                  << " priority=" << saturn::sh2::priorityName(site.priority) << '\n';
    }

    std::cout << "\nUnresolved indirect transfers:\n";
    if (report.unresolvedIndirectTransfers.empty()) {
        std::cout << "  none\n";
    }
    for (const auto& transfer : report.unresolvedIndirectTransfers) {
        std::cout << "  " << saturn::sh2::formatAddress(transfer.address)
                  << " block=" << saturn::sh2::formatAddress(transfer.block)
                  << " flow=" << saturn::sh2::flowName(transfer.flow) << '\n';
    }


    std::cout << "\nDelay-slot issues:\n";
    if (report.delaySlotIssues.empty()) {
        std::cout << "  none\n";
    }
    for (const auto& issue : report.delaySlotIssues) {
        std::cout << "  transfer=" << saturn::sh2::formatAddress(issue.transferAddress)
                  << " slot=";
        if (issue.slotAddress) {
            std::cout << saturn::sh2::formatAddress(*issue.slotAddress);
        } else {
            std::cout << "<outside-image>";
        }
        std::cout << " block=" << saturn::sh2::formatAddress(issue.block)
                  << " issue=" << saturn::sh2::delaySlotIssueName(issue.kind) << '\n';
    }
    return 0;
}

std::string_view programStatusName(saturn::sir::ProgramStatus status) {
    switch (status) {
    case saturn::sir::ProgramStatus::Returned: return "returned";
    case saturn::sir::ProgramStatus::StepLimit: return "step-limit";
    case saturn::sir::ProgramStatus::MissingEntry: return "missing-entry";
    case saturn::sir::ProgramStatus::UnresolvedTarget: return "unresolved-target";
    case saturn::sir::ProgramStatus::InterpreterFailure: return "interpreter-failure";
    case saturn::sir::ProgramStatus::Trap: return "trap";
    case saturn::sir::ProgramStatus::ExceptionReturn: return "exception-return";
    case saturn::sir::ProgramStatus::Sleeping: return "sleeping";
    case saturn::sir::ProgramStatus::FrontendQuit: return "frontend-quit";
    }
    return "unknown";
}

void loadState(const saturn::core::MemoryMap& image, saturn::sir::MachineState& state) {
    for (const auto& [_, segment] : image.segments()) {
        for (std::size_t offset = 0; offset < segment.bytes.size(); ++offset) {
            const auto address = static_cast<saturn::core::Address>(segment.base + offset);
            state.write8(address, std::to_integer<std::uint8_t>(segment.bytes[offset]));
        }
    }
}

bool writeFramePpm(const std::filesystem::path& path,
                   const saturn::platform::VideoFrame& frame) {
    std::ofstream output(path, std::ios::binary);
    if (!output) return false;
    output << "P6\n" << frame.width << ' ' << frame.height << "\n255\n";
    for (const auto pixel : frame.pixels) {
        const auto red = static_cast<char>((pixel >> 16U) & 0xFFU);
        const auto green = static_cast<char>((pixel >> 8U) & 0xFFU);
        const auto blue = static_cast<char>(pixel & 0xFFU);
        output.write(&red, 1);
        output.write(&green, 1);
        output.write(&blue, 1);
    }
    return static_cast<bool>(output);
}

bool writeVideoDiagnostics(const std::filesystem::path& directory,
                           const saturn::platform::VideoDiagnosticSnapshot& snapshot) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    std::ostringstream stem;
    stem << "frame_" << std::setw(6) << std::setfill('0') << snapshot.composed.sequence;
    const auto base = directory / stem.str();
    if (!snapshot.vramDump.empty()) {
        const auto dump = [&](const char* suffix, const auto& bytes) {
            std::ofstream file(base.string() + suffix, std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
            return static_cast<bool>(file);
        };
        if (!dump("_vram.bin", snapshot.vramDump) ||
            !dump("_cram.bin", snapshot.cramDump) ||
            !dump("_vdp1_vram.bin", snapshot.vdp1VramDump) ||
            !dump("_registers.bin", snapshot.registerDump)) return false;
        std::ofstream writes(base.string() + "_writes.csv");
        writes << "memory,offset_hex,writes,last_frame\n";
        const auto regions = [&](const char* memory, const auto& entries) {
            for (std::size_t i = 0; i < entries.size(); ++i) {
                writes << memory << ',' << std::hex << i * 32U << std::dec << ','
                       << entries[i].writes << ',' << entries[i].lastFrame << '\n';
            }
        };
        regions("vram", snapshot.vramWrites);
        regions("cram", snapshot.cramWrites);
        std::ofstream cells(base.string() + "_cells.csv");
        cells << "x,y,plane,map_hex,name_upper_hex,name_lower_hex,character_hex,palette_hex,"
                 "character_address_hex,raw_character_hex,cram_address_hex,raw_cram_hex,"
                 "decoded_argb_hex,window_allows,nbg0_argb_hex,vdp2_argb_hex,final_argb_hex\n";
        for (const auto& s : snapshot.cellSamples) {
            const auto i = static_cast<std::size_t>(s.y) * snapshot.nbg0.width + s.x;
            cells << std::dec << s.x << ',' << s.y << ',' << s.plane << ',' << std::hex
                  << s.mapAddress << ',' << s.nameUpper << ',' << s.nameLower << ','
                  << s.character << ',' << s.palette << ',' << s.characterAddress << ','
                  << s.rawCharacter << ',' << s.cramAddress << ',' << s.rawCram << ','
                  << s.color << ',' << s.windowAllows << ',' << snapshot.nbg0.pixels[i] << ','
                  << snapshot.vdp2.pixels[i] << ',' << snapshot.composed.pixels[i] << '\n';
        }
        if (!writes || !cells) return false;
    }
    {
        std::ofstream textureWrites(base.string() + "_vdp1-memory-writes.csv");
        if (!textureWrites) return false;
        textureWrites << "step,block,caller,offset_hex,width,value_hex,registers_available";
        for (unsigned index = 0U; index < 8U; ++index) {
            textureWrites << ",r" << index << "_hex";
        }
        textureWrites << '\n';
        for (const auto& write : snapshot.vdp1MemoryWriteHistory) {
            textureWrites << write.step << ','
                << saturn::sh2::formatAddress(write.blockAddress) << ','
                << saturn::sh2::formatAddress(write.callerBlock) << ",0x"
                << std::hex << write.offset << std::dec << ','
                << static_cast<unsigned>(write.width) << ",0x"
                << std::hex << write.value << std::dec << ','
                << (write.registersAvailable ? 1 : 0);
            for (const auto reg : write.registers) {
                if (write.registersAvailable) textureWrites << ",0x" << std::hex << reg;
                else textureWrites << ',';
            }
            textureWrites << std::dec << '\n';
        }
        if (!textureWrites) return false;
    }
    if (!writeFramePpm(base.string() + "_vdp2.ppm", snapshot.vdp2) ||
        !writeFramePpm(base.string() + "_back.ppm", snapshot.back) ||
        !writeFramePpm(base.string() + "_nbg0.ppm", snapshot.nbg0) ||
        !writeFramePpm(base.string() + "_nbg1.ppm", snapshot.vdp2Layers[1]) ||
        !writeFramePpm(base.string() + "_nbg2.ppm", snapshot.vdp2Layers[2]) ||
        !writeFramePpm(base.string() + "_nbg3.ppm", snapshot.vdp2Layers[3]) ||
        !writeFramePpm(base.string() + "_vdp1_bank0.ppm", snapshot.vdp1Banks[0]) ||
        !writeFramePpm(base.string() + "_vdp1_bank1.ppm", snapshot.vdp1Banks[1]) ||
        !writeFramePpm(base.string() + "_final.ppm", snapshot.composed)) {
        return false;
    }
    std::ofstream output(base.string() + ".txt");
    if (!output) return false;
    const auto writeHex = [&output](std::string_view name, std::uint16_t value) {
        output << name << "=0x" << std::hex << std::setw(4) << std::setfill('0') << value
               << std::dec << '\n';
    };
    const auto writePixels = [&output](std::string_view name,
                                       const saturn::platform::VideoPixelBounds& bounds) {
        output << name << ".non_zero=" << bounds.nonZeroPixels;
        if (bounds.nonZeroPixels != 0U) {
            output << " bounds=" << bounds.minX << ',' << bounds.minY << '-'
                   << bounds.maxX << ',' << bounds.maxY;
        }
        output << '\n';
    };
    const auto writeMemory = [&output](std::string_view name,
                                       const saturn::platform::VideoMemoryBounds& bounds) {
        output << name << ".non_zero_bytes=" << bounds.nonZeroBytes;
        if (bounds.nonZeroBytes != 0U) {
            output << " range=0x" << std::hex << bounds.firstByte << "-0x"
                   << bounds.lastByte << std::dec;
        }
        output << '\n';
    };
    output << "sequence=" << snapshot.composed.sequence << '\n'
           << "vdp1.modeled_banks=2\n"
           << "vdp1.display_bank=" << static_cast<unsigned>(snapshot.vdp1DisplayBank)
           << " draw_bank=" << static_cast<unsigned>(snapshot.vdp1DrawBank)
           << " swap_pending=" << (snapshot.vdp1SwapPending ? 1 : 0) << '\n';
    writePixels("vdp1.bank0", snapshot.vdp1Bounds[0]);
    writePixels("vdp1.bank1", snapshot.vdp1Bounds[1]);
    writePixels("vdp2.composed", snapshot.vdp2Bounds);
    writePixels("vdp2.back", snapshot.backBounds);
    writePixels("vdp2.nbg0", snapshot.nbg0Bounds);
    for (std::size_t layer = 1; layer < snapshot.vdp2LayerBounds.size(); ++layer) {
        writePixels("vdp2.nbg" + std::to_string(layer), snapshot.vdp2LayerBounds[layer]);
    }
    writePixels("final.composed", snapshot.composedBounds);
    writeMemory("vdp1.framebuffer", snapshot.vdp1Memory);
    writeMemory("vdp2.vram", snapshot.vdp2VramMemory);
    writeMemory("vdp2.cram", snapshot.vdp2CramMemory);
    output << "vdp2.BACK.address=0x" << std::hex << snapshot.vdp2BackAddress
           << " word=0x" << snapshot.vdp2BackWord << std::dec
           << " mode=" << (snapshot.vdp2BackLineMode ? "line" : "single") << '\n';
    output << "vdp2.BACK.samples=";
    for (std::size_t index = 0; index < snapshot.vdp2BackLineWords.size(); ++index) {
        if (index != 0U) output << ',';
        output << "0x" << std::hex << snapshot.vdp2BackLineWords[index];
    }
    output << std::dec << '\n';
    const bool bitmapNbg0 = (snapshot.vdp2Chctla & 0x0002U) != 0U;
    output << "vdp2.NBG0.addressing=" << (bitmapNbg0 ? "bitmap" : "cell") << '\n';
    if (bitmapNbg0) {
    output << "vdp2.NBG0.base=0x" << std::hex << snapshot.vdp2Nbg0Base << std::dec << '\n'
           << "vdp2.NBG0.width=" << snapshot.vdp2Nbg0Width
           << " height=" << snapshot.vdp2Nbg0Height
           << " bits_per_pixel=" << snapshot.vdp2Nbg0BitsPerPixel
           << " row_pitch=" << snapshot.vdp2Nbg0RowPitch << '\n';
    for (std::size_t line = 0; line < snapshot.vdp2Nbg0LineFirstRaw.size(); ++line) {
        output << "vdp2.NBG0.line[" << line << "].first_raw=0x"
               << std::hex << snapshot.vdp2Nbg0LineFirstRaw[line] << std::dec
               << " raw_hash=0x" << std::hex << snapshot.vdp2Nbg0LineRawHash[line]
               << std::dec
               << " raw_non_zero=" << snapshot.vdp2Nbg0LineRawNonZero[line]
               << " alpha_non_zero=" << snapshot.vdp2Nbg0LineAlphaNonZero[line]
               << " non_zero=" << snapshot.vdp2Nbg0LineNonZero[line] << '\n';
    }
    }
    writeHex("vdp1.TVMR", snapshot.vdp1Tvmr);
    writeHex("vdp1.FBCR", snapshot.vdp1Fbcr);
    writeHex("vdp1.PTMR", snapshot.vdp1Ptmr);
    writeHex("vdp1.EWDR", snapshot.vdp1Ewdr);
    writeHex("vdp1.EWLR", snapshot.vdp1Ewlr);
    writeHex("vdp1.EWRR", snapshot.vdp1Ewrr);
    writeHex("vdp1.EDSR", snapshot.vdp1Edsr);
    writeHex("vdp1.LOPR", snapshot.vdp1Lopr);
    writeHex("vdp1.COPR", snapshot.vdp1Copr);
    writeHex("vdp2.TVMD", snapshot.vdp2Tvmd);
    writeHex("vdp2.BGON", snapshot.vdp2Bgon);
    writeHex("vdp2.CHCTLA", snapshot.vdp2Chctla);
    writeHex("vdp2.CHCTLB", snapshot.vdp2Chctlb);
    writeHex("vdp2.BMPNA", snapshot.vdp2Bmpna);
    writeHex("vdp2.BMPNB", snapshot.vdp2Bmpnb);
    writeHex("vdp2.MPOFN", snapshot.vdp2Mpofn);
    writeHex("vdp2.MPOFR", snapshot.vdp2Mpofr);
    writeHex("vdp2.PRINA", snapshot.vdp2Prina);
    if (!snapshot.registerDump.empty()) {
        output << "vdp2.priority_configured=" << snapshot.priorityConfigured << '\n';
        for (std::size_t offset = 0; offset + 1 < snapshot.registerDump.size(); offset += 2) {
            output << "vdp2.register[0x" << std::hex << offset << "]=0x"
                   << ((snapshot.registerDump[offset] << 8U) | snapshot.registerDump[offset + 1])
                   << std::dec << '\n';
        }
    }
    writeHex("vdp2.PRINB", snapshot.vdp2Prinb);
    writeHex("vdp2.PRIR", snapshot.vdp2Prir);
    writeHex("vdp2.SPCTL", snapshot.vdp2Spctl);
    writeHex("vdp2.CCCTL", snapshot.vdp2Ccctl);
    writeHex("vdp2.WCTLA", snapshot.vdp2Wctla);
    writeHex("vdp2.WPSX0", snapshot.vdp2Wpsx0);
    writeHex("vdp2.WPSY0", snapshot.vdp2Wpsy0);
    writeHex("vdp2.WPEX0", snapshot.vdp2Wpex0);
    writeHex("vdp2.WPEY0", snapshot.vdp2Wpey0);
    writeHex("vdp2.SCRCTL", snapshot.vdp2Scrctl);
    writeHex("vdp2.VCSTAU", snapshot.vdp2Vcstau);
    writeHex("vdp2.VCSTAL", snapshot.vdp2Vcstal);
    writeHex("vdp2.LSTA0U", snapshot.vdp2Lsta0u);
    writeHex("vdp2.LSTA0L", snapshot.vdp2Lsta0l);
    const auto formatWords = [&output](std::string_view name,
                                       const std::vector<std::uint16_t>& words) {
        output << name << '=';
        for (std::size_t index = 0; index < words.size(); ++index) {
            if (index != 0U) output << ',';
            output << "0x" << std::hex << std::setw(4) << std::setfill('0')
                   << words[index];
        }
        output << std::dec << '\n';
    };
    formatWords("vdp2.NBG0.line_scroll_words", snapshot.vdp2LineScrollWords);
    formatWords("vdp2.NBG0.vertical_cell_scroll_words",
                snapshot.vdp2VerticalCellScrollWords);
    const auto formatTableAddress = [&output](std::string_view name,
                                               std::uint16_t upper,
                                               std::uint16_t lower) {
        const auto address = static_cast<std::uint32_t>(
            (((upper & 0x0007U) << 16U) | (lower & 0xFFFEU)) * 2U);
        output << name << "=0x" << std::hex << address << std::dec << '\n';
    };
    formatTableAddress("vdp2.NBG0.line_scroll_address",
                      snapshot.vdp2Lsta0u, snapshot.vdp2Lsta0l);
    formatTableAddress("vdp2.NBG0.vertical_cell_scroll_address",
                      snapshot.vdp2Vcstau, snapshot.vdp2Vcstal);
    output << "vdp2.NBG0.line_scroll_non_zero_words="
           << snapshot.vdp2LineScrollNonZeroWords
           << " first_index=" << snapshot.vdp2LineScrollFirstNonZero << '\n';
    output << "vdp2.NBG0.vertical_cell_scroll_non_zero_words="
           << snapshot.vdp2VerticalCellScrollNonZeroWords
           << " first_index=" << snapshot.vdp2VerticalCellScrollFirstNonZero << '\n';
    output << "vdp2.NBG0.window.enabled="
           << (((snapshot.vdp2Wctla & 0x0002U) != 0U) ? 1 : 0)
           << " inside=" << (((snapshot.vdp2Wctla & 0x0001U) != 0U) ? 1 : 0)
           << " outside=" << (((snapshot.vdp2Wctla & 0x0001U) == 0U) ? 1 : 0)
           << " rect=" << snapshot.vdp2Wpsx0 << ',' << snapshot.vdp2Wpsy0 << '-'
           << snapshot.vdp2Wpex0 << ',' << snapshot.vdp2Wpey0 << '\n';
    if (!output) return false;
    if (!snapshot.vdp1CommandHistory.empty()) {
        std::ofstream commands(directory / "vdp1-command-executions.txt");
        if (!commands) return false;
        commands << "# Captured command words in VDP1 traversal order; the final entry may be the END command.\n";
        for (std::size_t executionIndex = 0;
             executionIndex < snapshot.vdp1CommandHistory.size(); ++executionIndex) {
            const auto& execution = snapshot.vdp1CommandHistory[executionIndex];
            commands << "execution=" << executionIndex
                     << " draw_frame=" << execution.drawFrame
                     << " presented=" << (execution.wasPresented ? 1 : 0)
                     << " presented_frame=" << execution.presentedFrame
                     << " draw_bank=" << static_cast<unsigned>(execution.drawBank)
                     << " display_bank_at_draw="
                     << static_cast<unsigned>(execution.displayBankAtDraw)
                     << " displayed_bank=" << static_cast<unsigned>(execution.displayedBank)
                     << " FBCR=0x" << std::hex << execution.fbcr
                     << " PTMR=0x" << execution.ptmr << std::dec
                     << " commands=" << execution.stats.commands
                     << " sprites=" << execution.stats.sprites
                     << " polygons=" << execution.stats.polygons
                     << " lines=" << execution.stats.lines
                     << " skipped=" << execution.stats.skipped
                     << " malformed=" << execution.stats.malformed
                     << " last=0x" << std::hex << execution.stats.lastAddress
                     << std::dec << '\n';
            for (std::size_t commandIndex = 0;
                 commandIndex < execution.commands.size(); ++commandIndex) {
                const auto& command = execution.commands[commandIndex];
                commands << "  index=" << commandIndex << " address=0x"
                         << std::hex << command.address << " words=";
                for (std::size_t word = 0; word < command.words.size(); ++word) {
                    if (word != 0U) commands << ',';
                    commands << "0x" << std::setw(4) << std::setfill('0')
                             << command.words[word];
                }
                commands << std::dec << '\n';
            }
        }
        if (!commands) return false;
    }
    return true;
}

bool writeVdp2WindowTrace(
    const std::filesystem::path& directory,
    const std::deque<saturn::platform::MmioAccess>& accesses) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    std::ofstream output(directory / "vdp2-window-writes.txt");
    if (!output) return false;
    std::size_t ordinal{};
    for (const auto& access : accesses) {
        if (access.space != saturn::platform::AddressSpace::Vdp2 || !access.write) continue;
        const auto offset = access.address & 0x000FFFFFU;
        if (offset < 0x800C0U || offset > 0x800D0U) continue;
        const auto registerOffset = offset - 0x80000U;
        output << ordinal++ << " step=" << access.context.step
               << " block=" << saturn::sh2::formatAddress(access.context.blockAddress)
               << " caller=" << saturn::sh2::formatAddress(access.context.callerBlock)
               << " address=" << saturn::sh2::formatAddress(access.address)
               << " width=" << static_cast<unsigned>(access.width)
               << " reg=0x" << std::hex << registerOffset
               << " value=0x" << access.value << std::dec << '\n';
    }
    return static_cast<bool>(output);
}

bool writeScuDmaTrace(
    const std::filesystem::path& directory,
    const std::deque<saturn::platform::MmioAccess>& accesses) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    std::ofstream output(directory / "scu-dma-writes.txt");
    if (!output) return false;
    struct DmaDescriptorTrace {
        std::uint32_t source{};
        std::uint32_t destination{};
        std::uint32_t count{};
        std::uint32_t add{};
        std::uint32_t mode{};
        std::uint64_t sourceStep{};
        std::uint64_t destinationStep{};
        std::uint64_t countStep{};
        std::uint64_t goStep{};
        std::uint32_t goBlock{};
        std::uint32_t goCaller{};
    };
    std::array<DmaDescriptorTrace, 3> descriptors{};
    std::vector<std::string> nbg0Summaries;
    std::size_t ordinal{};
    for (const auto& access : accesses) {
        if (access.space != saturn::platform::AddressSpace::Scu || !access.write) continue;
        const auto offset = access.address & 0x0000FFFFU;
        const auto dmaRegister = offset <= 0x54U || offset == 0x98U ||
                                 offset == 0x9CU;
        if (!dmaRegister) continue;
        output << ordinal++ << " step=" << access.context.step
               << " block=" << saturn::sh2::formatAddress(access.context.blockAddress)
               << " caller=" << saturn::sh2::formatAddress(access.context.callerBlock)
               << " address=" << saturn::sh2::formatAddress(access.address)
               << " width=" << static_cast<unsigned>(access.width)
               << " offset=0x" << std::hex << offset
               << " value=0x" << access.value << std::dec << '\n';

        // The three DMA channels use 0x20-byte register windows. Keep a
        // compact reconstruction of each descriptor so the report makes the
        // effective NBG0 transfer length visible without scanning the raw log.
        if (offset <= 0x54U && access.width == 4U) {
            const auto channel = static_cast<std::size_t>(offset / 0x20U);
            const auto registerOffset = offset & 0x1FU;
            if (channel < descriptors.size()) {
                auto& descriptor = descriptors[channel];
                switch (registerOffset) {
                case 0x00U:
                    descriptor.source = access.value;
                    descriptor.sourceStep = access.context.step;
                    break;
                case 0x04U:
                    descriptor.destination = access.value;
                    descriptor.destinationStep = access.context.step;
                    break;
                case 0x08U:
                    descriptor.count = access.value;
                    descriptor.countStep = access.context.step;
                    break;
                case 0x0CU:
                    descriptor.add = access.value;
                    break;
                case 0x14U:
                    descriptor.mode = access.value;
                    break;
                case 0x10U:
                    if ((access.value & 1U) != 0U &&
                        descriptor.destination == 0x25E00000U) {
                        descriptor.goStep = access.context.step;
                        descriptor.goBlock = access.context.blockAddress;
                        descriptor.goCaller = access.context.callerBlock;
                        std::ostringstream summary;
                        summary << "summary.nbg0_dma channel=" << channel
                                << " step=" << descriptor.goStep
                                << " source=" << saturn::sh2::formatAddress(descriptor.source)
                                << " destination=" << saturn::sh2::formatAddress(descriptor.destination)
                                << " count=0x" << std::hex << descriptor.count
                                << " add=0x" << descriptor.add
                                << " mode=0x" << descriptor.mode << std::dec
                                << " source_step=" << descriptor.sourceStep
                                << " destination_step=" << descriptor.destinationStep
                                << " count_step=" << descriptor.countStep
                                << " block=" << saturn::sh2::formatAddress(descriptor.goBlock)
                                << " caller=" << saturn::sh2::formatAddress(descriptor.goCaller);
                        nbg0Summaries.push_back(summary.str());
                    }
                    break;
                default:
                    break;
                }
            }
        }
    }
    output << "# Reconstructed DMA descriptors targeting VDP2 VRAM base 0x25E00000\n";
    for (const auto& summary : nbg0Summaries) output << summary << '\n';
    return static_cast<bool>(output);
}

bool writeDmaDescriptorTrace(
    const std::filesystem::path& directory,
    const std::vector<DmaDescriptorRecord>& records) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    std::ofstream output(directory / "scu-dma-descriptors.txt");
    if (!output) return false;
    output << "# Descriptor snapshots at helper entry 0x06034A84\n";
    for (const auto& record : records) {
        output << "step=" << record.step
               << " block=" << saturn::sh2::formatAddress(record.block)
               << " caller=" << saturn::sh2::formatAddress(record.caller)
               << " descriptor=" << saturn::sh2::formatAddress(record.descriptor)
               << " source=" << saturn::sh2::formatAddress(record.fields[0])
               << " destination=" << saturn::sh2::formatAddress(record.fields[1])
               << " count=0x" << std::hex << record.fields[2]
               << " add_base=0x" << record.fields[3]
               << " add_flags=0x" << record.fields[4]
               << " mode=0x" << record.fields[8] << std::dec
               << " fields=";
        for (const auto value : record.fields) {
            output << saturn::sh2::formatAddress(value) << ',';
        }
        output << " source_samples=";
        for (std::size_t index = 0; index < record.sourceSamples.size(); ++index) {
            output << '+' << std::hex << kDmaSourceSampleOffsets[index]
                   << ':' << record.sourceSamples[index] << std::dec << ',';
        }
        output << '\n';
    }
    return static_cast<bool>(output);
}

bool writeScuDmaTransferTrace(
    const std::filesystem::path& directory,
    const std::vector<saturn::platform::ScuDmaRuntime::Transfer>& transfers,
    const std::vector<saturn::platform::ScuDmaRuntime::Transfer>& vdp1Textures,
    const std::vector<saturn::platform::ScuDmaRuntime::Transfer>& spriteSource) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    const auto writeTransfers = [&directory](
        const std::filesystem::path& filename,
        const char* header,
        const std::vector<saturn::platform::ScuDmaRuntime::Transfer>& records) {
        std::ofstream output(directory / filename);
        if (!output) return false;
        output << header << '\n';
        for (const auto& transfer : records) {
            output << "channel=" << transfer.channel
                   << " source=" << saturn::sh2::formatAddress(transfer.source)
                   << " destination=" << saturn::sh2::formatAddress(transfer.destination)
                   << " count=0x" << std::hex << transfer.count
                   << " add=0x" << transfer.add
                   << " mode=0x" << transfer.mode << std::dec
                   << " words=" << transfer.words
                   << " bytes=" << transfer.bytes
                   << " first_source=" << saturn::sh2::formatAddress(transfer.firstSource)
                   << " first_destination="
                   << saturn::sh2::formatAddress(transfer.firstDestination)
                   << " last_source=" << saturn::sh2::formatAddress(transfer.lastSource)
                   << " last_destination="
                   << saturn::sh2::formatAddress(transfer.lastDestination)
                   << " from_cd=" << (transfer.fromCd ? 1 : 0)
                   << " complete=" << (transfer.complete ? 1 : 0)
                   << " step=" << transfer.contextStep
                   << " block=" << saturn::sh2::formatAddress(transfer.contextBlock)
                   << " caller=" << saturn::sh2::formatAddress(transfer.contextCaller)
                   << '\n';
        }
        return static_cast<bool>(output);
    };
    return writeTransfers("scu-dma-transfers.txt",
                          "# Most recent SCU DMA transfers",
                          transfers) &&
           writeTransfers("scu-dma-vdp1-texture-transfers.txt",
                          "# SCU DMA transfers overlapping VDP1 texture VRAM",
                          vdp1Textures) &&
           writeTransfers("scu-dma-sprite-source-transfers.txt",
                          "# SCU DMA transfers writing the X sprite source buffer",
                          spriteSource);
}

bool writeSpriteSourceWriteTrace(const std::filesystem::path& directory,
                                 const SpriteSourceWriteProbe& probe) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    std::ofstream output(directory / "sprite-source-buffer-writes.csv");
    if (!output) return false;
    output << "# observed_writes=" << probe.writeCount()
           << " source_buffer_writes=" << probe.sourceWriteCount()
           << " offset_stores=" << probe.pointerWriteCount()
           << " recorded_rows=" << probe.recordedWriteCount()
           << " retained_rows=" << probe.writes().size() << '\n'
           << "step,block,caller,target,address,physical,width,previous_hex,value_hex\n";
    const auto& records = probe.writes();
    const auto start = probe.recordedWriteCount() > records.size() && !records.empty()
        ? probe.recordedWriteCount() % records.size() : 0U;
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto& record = records[(start + index) % records.size()];
        output << record.step << ','
               << saturn::sh2::formatAddress(record.block) << ','
               << saturn::sh2::formatAddress(record.caller) << ','
               << (record.pointerWrite ? "offset" : "source-buffer") << ','
               << saturn::sh2::formatAddress(record.address) << ','
               << saturn::sh2::formatAddress(record.physicalAddress) << ','
               << static_cast<unsigned>(record.width) << ",0x"
               << std::hex << record.previousValue << ",0x" << record.value
               << std::dec << '\n';
    }
    return static_cast<bool>(output);
}

bool writeCdSectorTrace(const std::filesystem::path& directory,
                        const saturn::platform::CdRuntime& cd) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;

    std::ofstream plays(directory / "cd-play-trace.csv");
    if (!plays) return false;
    plays << "# records=" << cd.playTrace().size()
          << " dropped=" << cd.droppedPlayTraceRecords() << '\n'
          << "sequence,cr1_hex,cr2_hex,cr3_hex,cr4_hex,start_fad,end_fad_exclusive,"
             "requested,available,playable,destination\n";
    for (const auto& record : cd.playTrace()) {
        plays << record.sequence;
        for (const auto value : record.registers) {
            plays << ",0x" << std::hex << value << std::dec;
        }
        plays << ',' << record.startFad << ',' << record.endFad << ','
              << record.requested << ',' << record.available << ','
              << record.playable << ','
              << static_cast<unsigned>(record.destination) << '\n';
    }
    if (!plays) return false;

    std::ofstream sectors(directory / "cd-sector-trace.csv");
    if (!sectors) return false;
    sectors << "# records=" << cd.sectorTrace().size()
            << " dropped=" << cd.droppedSectorTraceRecords() << '\n'
            << "sequence,source,fad,lba,play_cursor,partition,retained\n";
    for (const auto& record : cd.sectorTrace()) {
        sectors << record.sequence << ','
                << (record.source == 0U ? "play" : "get-sector") << ','
                << record.fad << ',' << record.lba << ',' << record.playCursor
                << ',' << static_cast<unsigned>(record.partition) << ','
                << (record.retained ? 1 : 0) << '\n';
    }
    return static_cast<bool>(sectors);
}

bool writeCdFifoReadTrace(const std::filesystem::path& directory,
                          const std::vector<CdFifoReadRecord>& records) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    std::ofstream output(directory / "cd-fifo-read-trace.csv");
    if (!output) return false;
    output << "# records=" << records.size()
           << " start_step=200000000 partition=1 instruction=0x61"
              " sampled_at_each_2048_byte_boundary\n"
           << "sequence,step,partition,instruction,transfer_lba,transfer_offset,"
              "transfer_count,fifo_offset_before,fifo_offset_after,address,width,"
              "value_hex,block,caller";
    for (std::size_t index = 0U; index < 16U; ++index) {
        output << ",r" << index;
    }
    output << '\n';
    for (std::size_t index = 0U; index < records.size(); ++index) {
        const auto& record = records[index];
        output << index + 1U << ',' << record.step << ','
               << static_cast<unsigned>(record.transferPartition) << ",0x"
               << std::hex << static_cast<unsigned>(record.transferInstruction)
               << std::dec << ',' << record.transferLba << ','
               << record.transferOffset << ',' << record.transferCount << ','
               << record.fifoOffsetBefore << ',' << record.fifoOffsetAfter << ','
               << saturn::sh2::formatAddress(record.address) << ','
               << static_cast<unsigned>(record.width) << ",0x" << std::hex
               << record.value << std::dec << ','
               << saturn::sh2::formatAddress(record.block) << ','
               << saturn::sh2::formatAddress(record.caller);
        for (const auto value : record.registers) {
            output << ',' << saturn::sh2::formatAddress(value);
        }
        output << '\n';
    }
    return static_cast<bool>(output);
}

bool writeControlTrace(
    const std::filesystem::path& path,
    const saturn::platform::MmioInterceptor& mmio) {
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return false;
    }
    std::ofstream output(path, std::ios::trunc);
    if (!output) return false;
    const auto& records = mmio.controlTrace();
    output << "# retained=" << records.size()
           << " dropped=" << mmio.droppedControlTraceRecords() << '\n'
           << "# CD status command 0x00 is omitted to keep polling volume bounded\n"
           << "sequence,step,kind,block,caller,address,width,value_hex,port,connected,"
              "cr1_hex,cr2_hex,cr3_hex,cr4_hex\n";
    for (const auto& record : records) {
        std::string_view kind;
        switch (record.kind) {
        case saturn::platform::ControlTraceKind::PadInput: kind = "pad-input"; break;
        case saturn::platform::ControlTraceKind::SmpcRead: kind = "smpc-read"; break;
        case saturn::platform::ControlTraceKind::SmpcWrite: kind = "smpc-write"; break;
        case saturn::platform::ControlTraceKind::CdCommand: kind = "cd-command"; break;
        }
        output << record.sequence << ',' << record.step << ',' << kind << ','
               << saturn::sh2::formatAddress(record.block) << ','
               << saturn::sh2::formatAddress(record.caller) << ','
               << saturn::sh2::formatAddress(record.address) << ','
               << static_cast<unsigned>(record.width) << ",0x"
               << std::hex << record.value << std::dec << ',';
        if (record.kind == saturn::platform::ControlTraceKind::PadInput) {
            output << static_cast<unsigned>(record.port) << ','
                   << (record.connected ? 1 : 0);
        } else {
            output << ',';
        }
        if (record.kind == saturn::platform::ControlTraceKind::CdCommand) {
            for (const auto reg : record.cdRegisters) {
                output << ",0x" << std::hex << reg << std::dec;
            }
        } else {
            output << ",,,,";
        }
        output << '\n';
    }
    return static_cast<bool>(output);
}

bool writeGfsSourceTrace(
    const std::filesystem::path& directory,
    const LaunchMemoryProbe& probe) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    std::ofstream output(directory / "gfs-source-writes.txt");
    if (!output) return false;
    output << "summary.gfs_source base="
           << saturn::sh2::formatAddress(kGfsSourceBase)
           << " size=0x" << std::hex << kGfsSourceSize << std::dec
           << " writes=" << probe.gfsSourceWriteCount()
           << " nonzero=" << probe.gfsSourceNonZeroWriteCount() << '\n';
    const auto& chunks = probe.gfsSourceChunks();
    for (std::size_t index = 0; index < chunks.size(); ++index) {
        const auto& chunk = chunks[index];
        if (chunk.writes == 0U) continue;
        output << "chunk=" << index
               << " offset=0x" << std::hex << index * kGfsSourceChunkSize
               << " writes=" << std::dec << chunk.writes
               << " nonzero=" << chunk.nonZeroWrites
               << " first_step=" << chunk.firstStep
               << " last_step=" << chunk.lastStep
               << " first_address=" << saturn::sh2::formatAddress(chunk.firstAddress)
               << " last_address=" << saturn::sh2::formatAddress(chunk.lastAddress)
               << " first_value=" << saturn::sh2::formatAddress(chunk.firstValue)
               << " last_value=" << saturn::sh2::formatAddress(chunk.lastValue)
               << " first_block=" << saturn::sh2::formatAddress(chunk.firstBlock)
               << " last_block=" << saturn::sh2::formatAddress(chunk.lastBlock)
               << " first_caller=" << saturn::sh2::formatAddress(chunk.firstCaller)
               << " last_caller=" << saturn::sh2::formatAddress(chunk.lastCaller)
               << '\n';
    }
    output << "boundary offset=0x45F00..0x46020\n";
    for (const auto& record : probe.gfsSourceBoundaryWrites()) {
        output << "step=" << record.context.step
               << " offset=0x" << std::hex
               << (record.address - kGfsSourceBase) << std::dec
               << " address=" << saturn::sh2::formatAddress(record.address)
               << " width=" << static_cast<unsigned>(record.width)
               << " value=" << saturn::sh2::formatAddress(record.value)
               << " block=" << saturn::sh2::formatAddress(record.context.blockAddress)
               << " caller=" << saturn::sh2::formatAddress(record.context.callerBlock)
               << '\n';
    }
    return static_cast<bool>(output);
}

bool writeGfsTaskConfigTrace(
    const std::filesystem::path& directory,
    const LaunchMemoryProbe& probe) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    std::ofstream output(directory / "gfs-task-config-writes.txt");
    if (!output) return false;
    output << "# GFS task object writes at 0x0606E45C (+58..+7C)\n";
    for (const auto& record : probe.gfsTaskConfigWrites()) {
        output << "step=" << record.context.step
               << " offset=0x" << std::hex
               << (record.address - kGfsTaskObjectAddress) << std::dec
               << " address=" << saturn::sh2::formatAddress(record.address)
               << " width=" << static_cast<unsigned>(record.width)
               << " value=" << saturn::sh2::formatAddress(record.value)
               << " block=" << saturn::sh2::formatAddress(record.context.blockAddress)
               << " caller=" << saturn::sh2::formatAddress(record.context.callerBlock)
               << '\n';
    }
    return static_cast<bool>(output);
}

bool writeGfsParserStackTrace(
    const std::filesystem::path& directory,
    const LaunchMemoryProbe& probe) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    std::ofstream output(directory / "gfs-parser-stack-writes.txt");
    if (!output) return false;
    output << "# Writes to the parser local slot @(4,R15) at 0x0604029A\n";
    for (const auto& record : probe.gfsParserStackWrites()) {
        output << "step=" << record.context.step
               << " address=" << saturn::sh2::formatAddress(record.address)
               << " width=" << static_cast<unsigned>(record.width)
               << " value=" << saturn::sh2::formatAddress(record.value)
               << " block=" << saturn::sh2::formatAddress(record.context.blockAddress)
               << " caller=" << saturn::sh2::formatAddress(record.context.callerBlock)
               << "\n";
    }
    return static_cast<bool>(output);
}

bool writeGfsParserDataTrace(
    const std::filesystem::path& directory,
    const LaunchMemoryProbe& probe) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    std::ofstream output(directory / "gfs-parser-data-writes.txt");
    if (!output) return false;
    output << "# Writes to parser object 0x0607C038 fields and table entry 0x0608021C\n";
    for (const auto& record : probe.gfsParserDataWrites()) {
        const auto address = record.address;
        const auto objectEnd = kGfsParserObjectAddress + 0x78U;
        const auto object = address >= kGfsParserObjectAddress && address < objectEnd;
        const auto region = object ? "object" : "table";
        const auto base = object ? kGfsParserObjectAddress : kGfsParserTableEntryAddress;
        output << "step=" << record.context.step
               << " region=" << region
               << " offset=0x" << std::hex << (address - base) << std::dec
               << " address=" << saturn::sh2::formatAddress(address)
               << " width=" << static_cast<unsigned>(record.width)
               << " value=" << saturn::sh2::formatAddress(record.value)
               << " block=" << saturn::sh2::formatAddress(record.context.blockAddress)
               << " caller=" << saturn::sh2::formatAddress(record.context.callerBlock)
               << " r0=" << saturn::sh2::formatAddress(contextRegister(
                   record.context, saturn::sh2::Register::R0))
               << " r1=" << saturn::sh2::formatAddress(contextRegister(
                   record.context, saturn::sh2::Register::R1))
               << " r2=" << saturn::sh2::formatAddress(contextRegister(
                   record.context, saturn::sh2::Register::R2))
               << " r4=" << saturn::sh2::formatAddress(contextRegister(
                   record.context, saturn::sh2::Register::R4))
               << " r5=" << saturn::sh2::formatAddress(contextRegister(
                   record.context, saturn::sh2::Register::R5))
               << " r13=" << saturn::sh2::formatAddress(contextRegister(
                   record.context, saturn::sh2::Register::R13))
               << " r14=" << saturn::sh2::formatAddress(contextRegister(
                   record.context, saturn::sh2::Register::R14))
               << '\n';
    }
    return static_cast<bool>(output);
}

bool writeGfsParserSourceTrace(
    const std::filesystem::path& directory,
    const LaunchMemoryProbe& probe) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    std::ofstream output(directory / "gfs-parser-source-reads.txt");
    if (!output) return false;
    output << "# Reads from CD Block FIFO 0x25818000/0x05818000 used for table copy\n";
    for (const auto& source : probe.gfsParserSourceReads()) {
        const auto& record = source.access;
        output << "step=" << record.context.step
               << " fifo_offset_before=" << source.fifoOffsetBefore
               << " fifo_offset_after=" << source.fifoOffsetAfter
               << " transfer_lba=" << source.transferLba
               << " transfer_fad=" << source.transferLba + 150U
               << " transfer_partition=" << static_cast<unsigned>(source.transferPartition)
               << " transfer_offset=" << source.transferOffset
               << " transfer_count=" << source.transferCount
               << " transfer_instruction=0x" << std::hex
               << static_cast<unsigned>(source.transferInstruction) << std::dec
               << " address=" << saturn::sh2::formatAddress(record.address)
               << " width=" << static_cast<unsigned>(record.width)
               << " value=" << saturn::sh2::formatAddress(record.value)
               << " block=" << saturn::sh2::formatAddress(record.context.blockAddress)
               << " caller=" << saturn::sh2::formatAddress(record.context.callerBlock)
               << " r0=" << saturn::sh2::formatAddress(contextRegister(
                   record.context, saturn::sh2::Register::R0))
               << " r2=" << saturn::sh2::formatAddress(contextRegister(
                   record.context, saturn::sh2::Register::R2))
               << " r5=" << saturn::sh2::formatAddress(contextRegister(
                   record.context, saturn::sh2::Register::R5))
               << " r13=" << saturn::sh2::formatAddress(contextRegister(
                   record.context, saturn::sh2::Register::R13))
               << " r14=" << saturn::sh2::formatAddress(contextRegister(
                   record.context, saturn::sh2::Register::R14))
               << '\n';
    }
    return static_cast<bool>(output);
}

bool writeGfsDecodeTrace(
    const std::filesystem::path& directory,
    const LaunchMemoryProbe& probe) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    std::ofstream output(directory / "gfs-decode-calls.txt");
    if (!output) return false;
    output << "# GFS parser/decoder/decompressor snapshots\n";
    const auto writeRecord = [&output](const GfsDecodeRecord& record) {
        output << "step=" << record.step
               << " block=" << saturn::sh2::formatAddress(record.block)
               << " caller=" << saturn::sh2::formatAddress(record.caller)
               << " sample_pointer=" << saturn::sh2::formatAddress(record.samplePointer)
               << " r4=" << saturn::sh2::formatAddress(record.registers[0])
               << " r5=" << saturn::sh2::formatAddress(record.registers[1])
               << " r6=" << saturn::sh2::formatAddress(record.registers[2])
               << " r7=" << saturn::sh2::formatAddress(record.registers[3])
               << " r8=" << saturn::sh2::formatAddress(record.registers[4])
               << " r9=" << saturn::sh2::formatAddress(record.registers[5])
               << " r10=" << saturn::sh2::formatAddress(record.registers[6])
               << " r11=" << saturn::sh2::formatAddress(record.registers[7])
               << " r12=" << saturn::sh2::formatAddress(record.registers[8])
               << " r13=" << saturn::sh2::formatAddress(record.registers[9])
               << " r14=" << saturn::sh2::formatAddress(record.registers[10])
               << " sp=" << saturn::sh2::formatAddress(record.registers[11])
               << " input_words=";
        for (const auto value : record.inputWords) {
            output << saturn::sh2::formatAddress(value) << ',';
        }
        output << " stack_words=";
        for (const auto value : record.stackWords) {
            output << saturn::sh2::formatAddress(value) << ',';
        }
        if (record.block == 0x06041D14U) {
            output << " task_object_words(+58..+7C)=";
            for (const auto value : record.taskObjectWords) {
                output << saturn::sh2::formatAddress(value) << ',';
            }
        }
        if (record.block == 0x0604045EU) {
            output << " parser_parameter_words(r9+0C/r9+10/r13+24/stack+04)=";
            for (const auto value : record.parserParameterWords) {
                output << saturn::sh2::formatAddress(value) << ',';
            }
        }
        if (record.block == 0x0604022CU || record.block == 0x06040246U ||
            record.block == 0x06040256U || record.block == 0x06040262U ||
            record.block == 0x06040284U || record.block == 0x0604029AU ||
            record.block == 0x0604029CU || record.block == 0x060402A0U) {
            output << " parser_calculation_words(r0/r1/r2/r3/r4/r5/r6/r13/r14/table0/table1/table2/table3/object+0C/object+14/object+64/object+6C/object+70)=";
            for (const auto value : record.parserCalculationWords) {
                output << saturn::sh2::formatAddress(value) << ',';
            }
        }
        output << '\n';
    };
    for (const auto& record : probe.gfsDecodeRecords()) writeRecord(record);
    output << "# Parser/setup/boundary snapshots (kept separately from loop cap)\n";
    for (const auto& record : probe.gfsDecodeBoundaryRecords()) {
        writeRecord(record);
    }
    output << "# R2 calculation and table-entry snapshots at 0x0604022C..0x060402A0\n";
    for (const auto& record : probe.gfsParserCalculationRecords()) {
        writeRecord(record);
    }
    return static_cast<bool>(output);
}

bool writeGfsStabCallbackTrace(
    const std::filesystem::path& directory,
    const LaunchMemoryProbe& probe) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    std::ofstream output(directory / "gfs-stab-callback.txt");
    if (!output) return false;

    output << "# STAB -> GFS parser callback -> decompressor correlation\n"
           << "# parser callback: 0x0604241A; task configurators: 0x06042698/0x06042702;\n"
           << "# decompressor entry: 0x06041D14; output boundary: 0x06041E9C\n";

    const GfsDecodeRecord* lastParser = nullptr;
    const auto writeLink = [&output, &lastParser]() {
        if (lastParser == nullptr) {
            output << " linked_parser_step=none";
            return;
        }
        output << " linked_parser_step=" << lastParser->step
               << " linked_descriptor="
               << saturn::sh2::formatAddress(lastParser->stabDescriptor)
               << " stab=";
        for (const auto value : lastParser->stabFields) {
            output << saturn::sh2::formatAddress(value) << ',';
        }
    };

    for (const auto& record : probe.gfsDecodeBoundaryRecords()) {
        if (record.block == 0x0604241AU) {
            lastParser = &record;
            output << "event=parser-callback"
                   << " step=" << record.step
                   << " parser_object=" << saturn::sh2::formatAddress(record.registers[10])
                   << " table=" << saturn::sh2::formatAddress(record.registers[9])
                   << " index=" << record.parserObjectIndex
                   << " descriptor=" << saturn::sh2::formatAddress(record.stabDescriptor)
                   << " stab_offset=" << saturn::sh2::formatAddress(record.stabFields[0])
                   << " stab_length=" << saturn::sh2::formatAddress(record.stabFields[1])
                   << " stab_flags=" << saturn::sh2::formatAddress(record.stabFields[2])
                   << " stab_type=" << saturn::sh2::formatAddress(record.stabFields[3])
                   << " gfs_record=" << saturn::sh2::formatAddress(record.registers[0])
                   << " parser_r10_length=" << saturn::sh2::formatAddress(record.registers[6])
                   << " parser_r7=" << saturn::sh2::formatAddress(record.registers[3])
                   << " gfs_type_word=" << saturn::sh2::formatAddress(record.inputWords[2])
                   << " packed_dimensions=" << saturn::sh2::formatAddress(record.inputWords[5])
                   << " derived_width=" << saturn::sh2::formatAddress(
                       record.inputWords[5] & 0xFFFFU)
                   << " derived_height=" << saturn::sh2::formatAddress(
                       record.inputWords[5] >> 16U)
                   << " gfs_words0_5=";
            for (std::size_t index = 0; index < 6U; ++index) {
                output << saturn::sh2::formatAddress(record.inputWords[index]) << ',';
            }
            output
                   << '\n';
            continue;
        }
        if (record.block == 0x06042530U || record.block == 0x06042590U ||
            record.block == 0x060425C2U) {
            output << "event=parser-field-transform"
                   << " step=" << record.step
                   << " block=" << saturn::sh2::formatAddress(record.block)
                   << " source=" << saturn::sh2::formatAddress(record.samplePointer)
                   << " r7=" << saturn::sh2::formatAddress(record.registers[3])
                   << " r8=" << saturn::sh2::formatAddress(record.registers[4])
                   << " r10=" << saturn::sh2::formatAddress(record.registers[6])
                   << " r11=" << saturn::sh2::formatAddress(record.registers[7])
                   << " r13=" << saturn::sh2::formatAddress(record.registers[9])
                   << " derived_stride=" << saturn::sh2::formatAddress(
                       record.registers[6])
                   << " transform_r7=" << saturn::sh2::formatAddress(
                       record.registers[3])
                   << " transform_r11=" << saturn::sh2::formatAddress(
                       record.registers[7])
                   << " gfs_words0_2=";
            for (std::size_t index = 0; index < 3U; ++index) {
                output << saturn::sh2::formatAddress(record.inputWords[index]) << ',';
            }
            output << " stack_word2="
                   << saturn::sh2::formatAddress(record.stackWords[2])
                   << '\n';
            continue;
        }
        if (record.block == 0x06042698U || record.block == 0x06042702U) {
            output << "event=task-config"
                   << " step=" << record.step
                   << " block=" << saturn::sh2::formatAddress(record.block)
                   << " task_object=" << saturn::sh2::formatAddress(record.samplePointer)
                   << " callback=" << saturn::sh2::formatAddress(record.inputWords[1])
                   << " source=" << saturn::sh2::formatAddress(record.registers[9])
                   << " stride=" << saturn::sh2::formatAddress(record.registers[6])
                   << " height=" << saturn::sh2::formatAddress(record.registers[3])
                   << " width=" << saturn::sh2::formatAddress(record.stackWords[2]);
            writeLink();
            output << '\n';
            continue;
        }
        if (record.block == 0x06041D14U) {
            output << "event=decompressor-entry"
                   << " step=" << record.step
                   << " task_object=" << saturn::sh2::formatAddress(record.samplePointer)
                   << " output=" << saturn::sh2::formatAddress(record.registers[0])
                   << " stride=" << saturn::sh2::formatAddress(record.registers[1])
                   << " width=" << saturn::sh2::formatAddress(record.registers[2])
                   << " height=" << saturn::sh2::formatAddress(record.registers[3])
                   << " compressed_source=" << saturn::sh2::formatAddress(
                       record.taskObjectWords[6]);
            writeLink();
            output << '\n';
            continue;
        }
        if (record.block == 0x06041E9CU || record.block == 0x06041EA6U) {
            output << "event=decompressor-boundary"
                   << " step=" << record.step
                   << " block=" << saturn::sh2::formatAddress(record.block)
                   << " output=" << saturn::sh2::formatAddress(record.registers[0])
                   << " stride=" << saturn::sh2::formatAddress(record.registers[1])
                   << " remaining_width=" << saturn::sh2::formatAddress(record.registers[2])
                   << " remaining_height=" << saturn::sh2::formatAddress(record.registers[3]);
            writeLink();
            output << '\n';
        }
    }
    return static_cast<bool>(output);
}

bool writeGfsRawFramePpm(const std::filesystem::path& directory,
                         const std::string& name,
                         const std::vector<std::uint32_t>& words) {
    constexpr std::size_t kWidth = 320U;
    constexpr std::size_t kHeight = 224U;
    if (words.size() != kWidth * kHeight) return false;
    std::ofstream output(directory / name, std::ios::binary);
    if (!output) return false;
    output << "P6\n" << kWidth << ' ' << kHeight << "\n255\n";
    for (const auto value : words) {
        const std::array<char, 3> pixel{
            static_cast<char>(value & 0xFFU),
            static_cast<char>((value >> 8U) & 0xFFU),
            static_cast<char>((value >> 16U) & 0xFFU),
        };
        output.write(pixel.data(), static_cast<std::streamsize>(pixel.size()));
    }
    return static_cast<bool>(output);
}

bool writeGfsOutputCoverageTrace(
    const std::filesystem::path& directory,
    const LaunchMemoryProbe& probe) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    std::ofstream output(directory / "gfs-output-coverage.txt");
    if (!output) return false;
    output << "# GFS decompressor output coverage sampled at 0x06041EA6\n"
           << "# non_zero_words scans the final RAM contents; written_words counts words\n"
           << "# written by this decoder call, avoiding stale bytes from an earlier resource\n"
           << "# row_hash is FNV-1a over the complete 320-word row at the decoder boundary\n"
           << "# pre_row_hash is the same hash captured before this decoder call\n"
           << "# changed_words counts slots whose complete 32-bit value differs pre/post\n"
           << "# unique_written_words counts distinct 32-bit output slots touched by this call\n"
           << "# skipped_changed_words counts unwritten slots that changed during the decoder call\n";
    std::size_t snapshotIndex = 0U;
    for (const auto& coverage : probe.gfsOutputCoverageRecords()) {
        if (!coverage.preFrameWords.empty()) {
            const auto suffix = std::to_string(snapshotIndex++);
            if (!writeGfsRawFramePpm(directory,
                                     "gfs-frame-" + suffix + "-pre.ppm",
                                     coverage.preFrameWords) ||
                !writeGfsRawFramePpm(directory,
                                     "gfs-frame-" + suffix + "-post.ppm",
                                     coverage.postFrameWords)) {
                return false;
            }
        }
        output << "decompressor-boundary step=" << coverage.step
               << " video_sequence=" << coverage.videoSequence
               << " pre_frame_hash=0x" << std::hex << coverage.preFrameHash
               << " post_frame_hash=0x" << coverage.postFrameHash << std::dec
               << " output=" << saturn::sh2::formatAddress(coverage.outputBase)
               << " compressed_source="
               << saturn::sh2::formatAddress(coverage.compressedSource)
               << " stride=" << saturn::sh2::formatAddress(coverage.stride)
               << " width=" << saturn::sh2::formatAddress(coverage.width)
               << " height=" << saturn::sh2::formatAddress(coverage.height)
               << " write_count=" << coverage.writeCount
               << " max_written_row=" << coverage.maxWrittenRow
               << " max_written_offset=0x" << std::hex << coverage.maxWrittenOffset
               << std::dec
               << " first_write=" << saturn::sh2::formatAddress(coverage.firstWrite)
               << " last_write=" << saturn::sh2::formatAddress(coverage.lastWrite)
               << " skipped_unchanged_words=" << coverage.skippedUnchangedWords
               << " skipped_changed_words=" << coverage.skippedChangedWords
               << " changed_word_count=" << coverage.changedWordCount
               << " rows=" << coverage.rows << '\n';
        for (std::uint32_t row = 0; row < coverage.rows; ++row) {
            output << " row=" << row
                   << " written_words=" << coverage.writtenWords[row]
                   << " written_non_zero_words=" << coverage.writtenNonZeroWords[row]
                   << " unique_written_words=" << coverage.uniqueWrittenWords[row]
                   << " changed_words=" << coverage.changedWords[row]
                   << " non_zero_words=" << coverage.nonZeroWords[row]
                   << " non_zero_rgb_words=" << coverage.nonZeroRgbWords[row]
                   << " pre_row_hash=0x" << std::hex << coverage.preRowHash[row]
                   << " row_hash=0x" << coverage.rowHash[row]
                   << std::dec
                   << " first=" << saturn::sh2::formatAddress(coverage.firstWord[row])
                   << " last=" << saturn::sh2::formatAddress(coverage.lastWord[row])
                   << '\n';
        }
    }
    return static_cast<bool>(output);
}

bool writeGfsSkipTrace(
    const std::filesystem::path& directory,
    const LaunchMemoryProbe& probe) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    std::ofstream output(directory / "gfs-skip-runs.txt");
    if (!output) return false;
    output << "# GFS delta skip visits at 0x0604205E; each visit advances four output pointers by 16 bytes\n"
           << "# command is R10 before the skip-loop shift; input_words sample the compressed stream\n";
    for (const auto& record : probe.gfsSkipRecords()) {
        output << "step=" << record.step
               << " input=" << saturn::sh2::formatAddress(record.input)
               << " output=" << saturn::sh2::formatAddress(record.output)
               << " command=" << saturn::sh2::formatAddress(record.command)
               << " inner_remaining=" << saturn::sh2::formatAddress(record.innerRemaining)
               << " row_remaining=" << saturn::sh2::formatAddress(record.rowRemaining)
               << " input_words=";
        for (std::size_t index = 0; index < record.inputWords.size(); ++index) {
            if (index != 0U) output << ',';
            output << saturn::sh2::formatAddress(record.inputWords[index]);
        }
        output << '\n';
    }
    return static_cast<bool>(output);
}

bool writeVdp2LayerTrace(
    const std::filesystem::path& directory,
    const std::deque<saturn::platform::MmioAccess>& accesses) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    std::ofstream output(directory / "vdp2-layer-writes.txt");
    if (!output) return false;
    std::size_t ordinal{};
    std::size_t nbg0Writes{};
    constexpr std::size_t kNbg0VramSize = 0x80000U;
    constexpr std::size_t kNbg0ChunkSize = 0x4000U; // eight 512-pixel rows at 32bpp
    struct Nbg0Chunk {
        std::size_t writes{};
        std::size_t nonZeroWrites{};
        std::size_t contentWrites{};
        std::uint64_t firstStep{};
        std::uint64_t lastStep{};
        std::uint64_t firstContentStep{};
        std::uint64_t lastContentStep{};
        std::uint32_t firstBlock{};
        std::uint32_t lastBlock{};
        std::uint32_t firstContentBlock{};
        std::uint32_t lastContentBlock{};
        std::uint32_t firstCaller{};
        std::uint32_t lastCaller{};
        std::uint32_t firstContentCaller{};
        std::uint32_t lastContentCaller{};
        std::uint32_t firstAddress{};
        std::uint32_t lastAddress{};
        std::uint32_t firstContentAddress{};
        std::uint32_t lastContentAddress{};
        std::uint8_t firstWidth{};
        std::uint8_t lastWidth{};
        std::uint8_t firstContentWidth{};
        std::uint8_t lastContentWidth{};
        std::uint32_t firstValue{};
        std::uint32_t lastValue{};
        std::uint32_t firstContentValue{};
        std::uint32_t lastContentValue{};
        std::array<std::uint32_t, 6> firstContentRegs{};
        std::array<std::uint32_t, 6> lastContentRegs{};
    };
    std::array<Nbg0Chunk, kNbg0VramSize / kNbg0ChunkSize> nbg0Chunks{};
    for (const auto& access : accesses) {
        if (access.space != saturn::platform::AddressSpace::Vdp2 || !access.write) continue;
        const auto offset = access.address & 0x000FFFFFU;
        const auto isRegister = offset >= 0x80000U && offset < 0x80200U;
        const auto registerOffset = isRegister ? offset - 0x80000U : 0U;
        const auto isLayerRegister = isRegister &&
            (registerOffset == 0x000U || registerOffset == 0x020U ||
             registerOffset == 0x028U || registerOffset == 0x02CU ||
             registerOffset == 0x03CU || registerOffset == 0x0ACU ||
             registerOffset == 0x0AEU || registerOffset == 0x0E0U ||
             registerOffset == 0x0ECU || registerOffset == 0x0F8U ||
             registerOffset == 0x0FAU || registerOffset == 0x0FCU ||
             registerOffset == 0x09AU || registerOffset == 0x09CU ||
             registerOffset == 0x09EU || registerOffset == 0x0A0U ||
             registerOffset == 0x0A2U);
        // BKTAU/BKTAL normally point at a small table near the end of VRAM.
        // Keep a focused window around that table instead of dumping the
        // hundreds of thousands of NBG0 bitmap writes.
        const auto isBackTableWrite = !isRegister && offset >= 0x6FF00U &&
                                      offset < 0x70100U;
        const auto isNbg0BoundaryWrite = !isRegister && offset >= 0x45F00U &&
                                         offset < 0x46100U;
        if (!isRegister && offset < kNbg0VramSize) {
            ++nbg0Writes;
            const auto chunkIndex = static_cast<std::size_t>(offset) / kNbg0ChunkSize;
            auto& chunk = nbg0Chunks[chunkIndex];
            if (chunk.writes == 0U) {
                chunk.firstStep = access.context.step;
                chunk.firstBlock = access.context.blockAddress;
                chunk.firstCaller = access.context.callerBlock;
                chunk.firstAddress = access.address;
                chunk.firstWidth = access.width;
                chunk.firstValue = access.value;
            }
            ++chunk.writes;
            if (access.value != 0U) ++chunk.nonZeroWrites;
            // Alpha-only values are useful for the decoder, but do not carry
            // RGB payload. Count them separately so a fill/status pattern is
            // distinguishable from actual bitmap content in the trace.
            const auto contentMask = access.width == 2U ? 0x7FFFU :
                access.width == 4U ? 0x7FFFFFFFU : 0xFFU;
            if ((access.value & contentMask) != 0U) {
                if (chunk.contentWrites == 0U) {
                    chunk.firstContentStep = access.context.step;
                    chunk.firstContentBlock = access.context.blockAddress;
                    chunk.firstContentCaller = access.context.callerBlock;
                    chunk.firstContentAddress = access.address;
                    chunk.firstContentWidth = access.width;
                    chunk.firstContentValue = access.value;
                    chunk.firstContentRegs = {
                        contextRegister(access.context, saturn::sh2::Register::R0),
                        contextRegister(access.context, saturn::sh2::Register::R4),
                        contextRegister(access.context, saturn::sh2::Register::R5),
                        contextRegister(access.context, saturn::sh2::Register::R12),
                        contextRegister(access.context, saturn::sh2::Register::R13),
                        contextRegister(access.context, saturn::sh2::Register::R14)};
                }
                ++chunk.contentWrites;
                chunk.lastContentStep = access.context.step;
                chunk.lastContentBlock = access.context.blockAddress;
                chunk.lastContentCaller = access.context.callerBlock;
                chunk.lastContentAddress = access.address;
                chunk.lastContentWidth = access.width;
                chunk.lastContentValue = access.value;
                chunk.lastContentRegs = {
                    contextRegister(access.context, saturn::sh2::Register::R0),
                    contextRegister(access.context, saturn::sh2::Register::R4),
                    contextRegister(access.context, saturn::sh2::Register::R5),
                    contextRegister(access.context, saturn::sh2::Register::R12),
                    contextRegister(access.context, saturn::sh2::Register::R13),
                    contextRegister(access.context, saturn::sh2::Register::R14)};
            }
            chunk.lastStep = access.context.step;
            chunk.lastBlock = access.context.blockAddress;
            chunk.lastCaller = access.context.callerBlock;
            chunk.lastAddress = access.address;
            chunk.lastWidth = access.width;
            chunk.lastValue = access.value;
        }
        if (!isLayerRegister && !isBackTableWrite && !isNbg0BoundaryWrite) continue;
        output << ordinal++ << " step=" << access.context.step
               << " block=" << saturn::sh2::formatAddress(access.context.blockAddress)
               << " caller=" << saturn::sh2::formatAddress(access.context.callerBlock)
               << " address=" << saturn::sh2::formatAddress(access.address)
               << " width=" << static_cast<unsigned>(access.width);
        if (isRegister) {
            output << " kind=register reg=0x" << std::hex << registerOffset;
        } else {
            output << " kind=back-vram offset=0x" << std::hex << offset;
        }
        output << " value=0x" << access.value << std::dec << '\n';
    }
    output << "summary.nbg0_bitmap_writes=" << nbg0Writes << '\n';
    for (std::size_t index = 0; index < nbg0Chunks.size(); ++index) {
        const auto& chunk = nbg0Chunks[index];
        if (chunk.writes == 0U) continue;
        output << "summary.nbg0_chunk index=" << index
               << " offset=0x" << std::hex << index * kNbg0ChunkSize
               << " end=0x" << (index + 1U) * kNbg0ChunkSize
               << " writes=" << std::dec << chunk.writes
               << " nonzero=" << chunk.nonZeroWrites
               << " content=" << chunk.contentWrites
               << " first_step=" << chunk.firstStep
               << " first_block=" << saturn::sh2::formatAddress(chunk.firstBlock)
               << " first_caller=" << saturn::sh2::formatAddress(chunk.firstCaller)
               << " first_address=" << saturn::sh2::formatAddress(chunk.firstAddress)
               << " first_width=" << static_cast<unsigned>(chunk.firstWidth)
               << " first_value=" << saturn::sh2::formatAddress(chunk.firstValue)
               << " last_step=" << chunk.lastStep
               << " last_block=" << saturn::sh2::formatAddress(chunk.lastBlock)
               << " last_caller=" << saturn::sh2::formatAddress(chunk.lastCaller)
               << " last_address=" << saturn::sh2::formatAddress(chunk.lastAddress)
               << " last_width=" << static_cast<unsigned>(chunk.lastWidth)
               << " last_value=" << saturn::sh2::formatAddress(chunk.lastValue)
               << " first_content_step=" << chunk.firstContentStep
               << " first_content_block=" << saturn::sh2::formatAddress(chunk.firstContentBlock)
               << " first_content_caller=" << saturn::sh2::formatAddress(chunk.firstContentCaller)
               << " first_content_address=" << saturn::sh2::formatAddress(chunk.firstContentAddress)
               << " first_content_width=" << static_cast<unsigned>(chunk.firstContentWidth)
               << " first_content_value=" << saturn::sh2::formatAddress(chunk.firstContentValue)
               << " last_content_step=" << chunk.lastContentStep
               << " last_content_block=" << saturn::sh2::formatAddress(chunk.lastContentBlock)
               << " last_content_caller=" << saturn::sh2::formatAddress(chunk.lastContentCaller)
               << " last_content_address=" << saturn::sh2::formatAddress(chunk.lastContentAddress)
               << " last_content_width=" << static_cast<unsigned>(chunk.lastContentWidth)
               << " last_content_value=" << saturn::sh2::formatAddress(chunk.lastContentValue)
               << " first_content_regs=r0:" << saturn::sh2::formatAddress(chunk.firstContentRegs[0])
               << ",r4:" << saturn::sh2::formatAddress(chunk.firstContentRegs[1])
               << ",r5:" << saturn::sh2::formatAddress(chunk.firstContentRegs[2])
               << ",r12:" << saturn::sh2::formatAddress(chunk.firstContentRegs[3])
               << ",r13:" << saturn::sh2::formatAddress(chunk.firstContentRegs[4])
               << ",r14:" << saturn::sh2::formatAddress(chunk.firstContentRegs[5])
               << " last_content_regs=r0:" << saturn::sh2::formatAddress(chunk.lastContentRegs[0])
               << ",r4:" << saturn::sh2::formatAddress(chunk.lastContentRegs[1])
               << ",r5:" << saturn::sh2::formatAddress(chunk.lastContentRegs[2])
               << ",r12:" << saturn::sh2::formatAddress(chunk.lastContentRegs[3])
               << ",r13:" << saturn::sh2::formatAddress(chunk.lastContentRegs[4])
               << ",r14:" << saturn::sh2::formatAddress(chunk.lastContentRegs[5])
               << '\n';
    }
    return static_cast<bool>(output);
}

// Trace the actual NBG0 pattern-name plane.  MmioAccess retains the CPU
// context even when a SCU DMA transfer reaches the VideoRuntime through the
// MachineState interceptor.  The transfer history is joined afterward so a
// write can be attributed to a DMA descriptor with its source range.
// Unmatched writes are not necessarily CPU stores: DMA history is bounded.
bool writeVdp2MapProducerTrace(
    const std::filesystem::path& directory,
    const std::vector<Vdp2MapWriteRecord>& accesses,
    const std::vector<saturn::platform::ScuDmaRuntime::Transfer>& transfers) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    std::ofstream output(directory / "vdp2-map-producer.csv");
    if (!output) return false;
    output << "ordinal,step,block,caller,address,offset,width,value,previous,changed,nonzero,"
              "r0,r4,r5,r12,r13,r14,origin,dma_channel,dma_source,dma_destination,"
              "dma_count,dma_add,dma_mode,dma_first_destination,dma_last_destination,"
              "dma_first_source,dma_last_source,dma_complete\n";
    const auto normalizedOffset = [](saturn::core::Address address) {
        return address & 0x000FFFFFU;
    };
    const auto addressInTransfer = [&](saturn::core::Address address,
                                       const saturn::platform::ScuDmaRuntime::Transfer& transfer) {
        const auto first = normalizedOffset(transfer.firstDestination);
        const auto last = normalizedOffset(transfer.lastDestination);
        const auto current = normalizedOffset(address);
        if (first <= last) return current >= first && current <= last;
        return current >= first || current <= last;
    };
    std::size_t ordinal{};
    std::size_t mapWrites{};
    std::size_t zeroWrites{};
    std::size_t nonZeroWrites{};
    std::size_t changedWrites{};
    std::size_t zeroOverNonZeroWrites{};
    std::size_t dmaWrites{};
    std::size_t unmatchedWrites{};
    for (const auto& record : accesses) {
        const auto& access = record.access;
        const auto offset = normalizedOffset(access.address);
        if (offset < 0x70000U || offset >= 0x72000U) continue;
        ++mapWrites;
        if (access.value == 0U) ++zeroWrites; else ++nonZeroWrites;
        if (access.value != record.previousValue) ++changedWrites;
        if (access.value == 0U && record.previousValue != 0U) {
            ++zeroOverNonZeroWrites;
        }
        const saturn::platform::ScuDmaRuntime::Transfer* match = nullptr;
        auto matchSpan = std::numeric_limits<std::uint64_t>::max();
        for (const auto& transfer : transfers) {
            if (!addressInTransfer(access.address, transfer)) continue;
            if (transfer.contextStep != 0U &&
                (transfer.contextStep != access.context.step ||
                 transfer.contextBlock != access.context.blockAddress ||
                 transfer.contextCaller != access.context.callerBlock)) continue;
            const auto first = normalizedOffset(transfer.firstDestination);
            const auto last = normalizedOffset(transfer.lastDestination);
            const auto span = first <= last
                ? static_cast<std::uint64_t>(last - first)
                : static_cast<std::uint64_t>(0x100000U - first + last);
            // A full-VRAM clear can cover every map address. Prefer the most
            // specific later descriptor when a map-sized transfer also fits.
            if (span <= matchSpan) {
                match = &transfer;
                matchSpan = span;
            }
        }
        const auto dma = match != nullptr;
        if (dma) ++dmaWrites; else ++unmatchedWrites;
        const auto reg = [&](saturn::sh2::Register name) {
            return contextRegister(access.context, name);
        };
        output << ordinal++ << ',' << access.context.step << ','
               << saturn::sh2::formatAddress(access.context.blockAddress) << ','
               << saturn::sh2::formatAddress(access.context.callerBlock) << ','
               << saturn::sh2::formatAddress(access.address) << ','
               << "0x" << std::hex << offset << std::dec << ','
               << static_cast<unsigned>(access.width) << ",0x" << std::hex
               << access.value << ",0x" << record.previousValue << std::dec << ','
               << (access.value != record.previousValue ? 1 : 0) << ','
               << (access.value != 0U ? 1 : 0)
               << ',' << saturn::sh2::formatAddress(reg(saturn::sh2::Register::R0))
               << ',' << saturn::sh2::formatAddress(reg(saturn::sh2::Register::R4))
               << ',' << saturn::sh2::formatAddress(reg(saturn::sh2::Register::R5))
               << ',' << saturn::sh2::formatAddress(reg(saturn::sh2::Register::R12))
               << ',' << saturn::sh2::formatAddress(reg(saturn::sh2::Register::R13))
               << ',' << saturn::sh2::formatAddress(reg(saturn::sh2::Register::R14))
               << ',' << (dma ? "scu-dma" : "unmatched");
        if (dma) {
            output << ',' << match->channel << ','
                   << saturn::sh2::formatAddress(match->source) << ','
                   << saturn::sh2::formatAddress(match->destination) << ",0x"
                   << std::hex << match->count << ",0x" << match->add << ",0x"
                   << match->mode << std::dec << ','
                   << saturn::sh2::formatAddress(match->firstDestination) << ','
                   << saturn::sh2::formatAddress(match->lastDestination) << ','
                   << saturn::sh2::formatAddress(match->firstSource) << ','
                   << saturn::sh2::formatAddress(match->lastSource) << ','
                   << (match->complete ? 1 : 0);
        } else {
            output << ",,,,,,,,,,,";
        }
        output << '\n';
    }
    output << "# summary.map_writes=" << mapWrites << '\n'
           << "# summary.zero_writes=" << zeroWrites << '\n'
           << "# summary.nonzero_writes=" << nonZeroWrites << '\n'
           << "# summary.changed_writes=" << changedWrites << '\n'
           << "# summary.zero_over_nonzero_writes=" << zeroOverNonZeroWrites << '\n'
           << "# summary.dma_writes=" << dmaWrites << '\n'
           << "# summary.unmatched_writes=" << unmatchedWrites << '\n'
           << "# map_range=0x70000-0x71fff\n";
    std::ofstream dmaOutput(directory / "vdp2-map-dma-transfers.csv");
    if (!dmaOutput) return false;
    dmaOutput << "index,channel,source,destination,count,add,mode,first_destination,last_destination,first_source,last_source,bytes,complete,context_step,context_block,context_caller\n";
    for (std::size_t index = 0; index < transfers.size(); ++index) {
        const auto& transfer = transfers[index];
        if (!addressInTransfer(0x25E70000U, transfer) &&
            !addressInTransfer(0x25E71FFEU, transfer)) continue;
        dmaOutput << index << ',' << transfer.channel << ','
                  << saturn::sh2::formatAddress(transfer.source) << ','
                  << saturn::sh2::formatAddress(transfer.destination) << ",0x"
                  << std::hex << transfer.count << ",0x" << transfer.add << ",0x"
                  << transfer.mode << std::dec << ','
                  << saturn::sh2::formatAddress(transfer.firstDestination) << ','
                  << saturn::sh2::formatAddress(transfer.lastDestination) << ','
                  << saturn::sh2::formatAddress(transfer.firstSource) << ','
                  << saturn::sh2::formatAddress(transfer.lastSource) << ','
                  << transfer.bytes << ',' << (transfer.complete ? 1 : 0) << ','
                  << transfer.contextStep << ','
                  << saturn::sh2::formatAddress(transfer.contextBlock) << ','
                  << saturn::sh2::formatAddress(transfer.contextCaller) << '\n';
    }
    return static_cast<bool>(output) && static_cast<bool>(dmaOutput);
}

bool writeVdp2MapReadTrace(
    const std::filesystem::path& directory,
    const std::vector<saturn::platform::VideoMapReadRecord>& records,
    const std::vector<saturn::platform::VideoMapStateRecord>& states) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    std::ofstream output(directory / "vdp2-map-read.csv");
    if (!output) return false;
    output << "frame,step,block,caller,renderer_reads,renderer_nonzero_reads,"
              "renderer_min_address,renderer_max_address,renderer_first_nonzero_address,"
              "renderer_first_nonzero_value,expected_words,expected_nonzero_words,"
              "expected_first_nonzero_address,expected_first_nonzero_value\n";
    for (const auto& record : records) {
        output << record.frame << ',' << record.contextStep << ','
               << saturn::sh2::formatAddress(record.contextBlock) << ','
               << saturn::sh2::formatAddress(record.contextCaller) << ','
               << record.rendererReads << ',' << record.rendererNonZeroReads << ','
               << saturn::sh2::formatAddress(static_cast<saturn::core::Address>(
                      record.rendererMinAddress)) << ','
               << saturn::sh2::formatAddress(static_cast<saturn::core::Address>(
                      record.rendererMaxAddress)) << ','
               << saturn::sh2::formatAddress(static_cast<saturn::core::Address>(
                      record.rendererFirstNonZeroAddress)) << ",0x"
               << std::hex << record.rendererFirstNonZeroValue << std::dec << ','
               << record.expectedWords << ',' << record.expectedNonZeroWords << ','
               << saturn::sh2::formatAddress(static_cast<saturn::core::Address>(
                      record.expectedFirstNonZeroAddress)) << ",0x"
               << std::hex << record.expectedFirstNonZeroValue << std::dec << '\n';
    }
    output << "# expected_map_range=0x70000-0x71fff\n"
           << "# rows=" << records.size() << '\n';
    std::ofstream stateOutput(directory / "vdp2-map-state.csv");
    if (!stateOutput) return false;
    stateOutput << "frame,step,block,caller,tvmd,bgon,chctla,mpofn,map_ab,"
                   "nbg0_enabled,cell_mode,expected_words,expected_nonzero_words,"
                   "expected_first_nonzero_address,expected_first_nonzero_value\n";
    for (const auto& state : states) {
        stateOutput << state.frame << ',' << state.contextStep << ','
                    << saturn::sh2::formatAddress(state.contextBlock) << ','
                    << saturn::sh2::formatAddress(state.contextCaller) << ",0x"
                    << std::hex << state.tvmd << ",0x" << state.bgon << ",0x"
                    << state.chctla << ",0x" << state.mpofn << ",0x" << state.mapAB
                    << std::dec << ',' << (state.nbg0Enabled ? 1 : 0) << ','
                    << (state.cellMode ? 1 : 0) << ',' << state.expectedWords << ','
                    << state.expectedNonZeroWords << ','
                    << saturn::sh2::formatAddress(static_cast<saturn::core::Address>(
                           state.expectedFirstNonZeroAddress)) << ",0x"
                    << std::hex << state.expectedFirstNonZeroValue << std::dec << '\n';
    }
    stateOutput << "# expected_map_range=0x70000-0x71fff\n"
                << "# rows=" << states.size() << '\n';
    return static_cast<bool>(output) && static_cast<bool>(stateOutput);
}

bool writeVdp1CommandTrace(const std::filesystem::path& directory,
                           const saturn::platform::VideoRuntime& video,
                           std::string_view artifactPrefix = {}) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    const auto commandListPath = directory /
        (std::string(artifactPrefix) + "vdp1-command-list.txt");
    const auto vramDumpPath = directory /
        (std::string(artifactPrefix) + "vdp1-vram.bin");
    std::ofstream output(commandListPath);
    if (!output) return false;
    std::vector<std::uint8_t> vdp1Vram(0x80000U);
    for (std::size_t offset = 0; offset < vdp1Vram.size(); ++offset) {
        vdp1Vram[offset] = static_cast<std::uint8_t>(video.read(
            saturn::platform::VideoRuntime::kVdp1Base +
                static_cast<saturn::core::Address>(offset), 1U).value_or(0U));
    }
    std::ofstream vramDump(vramDumpPath, std::ios::binary);
    if (!vramDump) return false;
    vramDump.write(reinterpret_cast<const char*>(vdp1Vram.data()),
                   static_cast<std::streamsize>(vdp1Vram.size()));
    if (!vramDump) return false;
    const auto wordFromDump = [&vdp1Vram](std::size_t offset) {
        if (offset + 1U >= vdp1Vram.size()) return std::uint16_t{};
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(vdp1Vram[offset]) << 8U) |
            static_cast<std::uint16_t>(vdp1Vram[offset + 1U]));
    };
    output << "# VDP1 commands in hardware traversal order; full VRAM is in vdp1-vram.bin\n";
    std::size_t offset{};
    std::size_t returnAddress = static_cast<std::size_t>(-1);
    std::size_t commandCount{};
    std::size_t skippedCount{};
    std::size_t spriteCount{};
    std::size_t polygonCount{};
    std::size_t lineCount{};
    std::size_t stateCount{};
    std::size_t malformedCount{};
    for (std::size_t index = 0; index < 2000U; ++index) {
        if (offset + 0x20U > vdp1Vram.size()) {
            ++malformedCount;
            output << "# malformed=out-of-range offset=0x" << std::hex << offset
                   << std::dec << '\n';
            break;
        }
        const auto control = wordFromDump(offset);
        if ((control & 0x8000U) != 0U) {
            output << "index=" << index << " offset=0x" << std::hex << offset
                   << " control=0x" << control << std::dec << " action=end\n";
            break;
        }
        ++commandCount;
        const auto linkWord = wordFromDump(offset + 0x02U);
        const auto pmod = wordFromDump(offset + 0x04U);
        const auto color = wordFromDump(offset + 0x06U);
        const auto sourceCell = wordFromDump(offset + 0x08U);
        const auto size = wordFromDump(offset + 0x0AU);
        const auto sourceWidth = static_cast<unsigned>(((size >> 8U) & 0x003FU) * 8U);
        const auto sourceHeight = static_cast<unsigned>(size & 0x00FFU);
        const auto colorMode = static_cast<unsigned>((pmod >> 3U) & 0x7U);
        const auto shape = static_cast<unsigned>(control & 0x000FU);
        const auto skipped = (control & 0x4000U) != 0U;
        std::string_view action = "ignored";
        if (skipped) {
            ++skippedCount;
            action = "skip";
        } else if (shape == 8U || shape == 9U) {
            ++stateCount;
            action = "clip";
        } else if (shape == 10U) {
            ++stateCount;
            action = "local-coordinate";
        } else if (shape <= 2U) {
            ++spriteCount;
            action = "sprite";
        } else if (shape == 4U) {
            ++polygonCount;
            action = "polygon";
        } else if (shape == 6U) {
            ++lineCount;
            action = "line";
        }
        const auto xa = wordFromDump(offset + 0x0CU);
        const auto ya = wordFromDump(offset + 0x0EU);
        const auto xb = wordFromDump(offset + 0x10U);
        const auto yb = wordFromDump(offset + 0x12U);
        const auto xc = wordFromDump(offset + 0x14U);
        const auto yc = wordFromDump(offset + 0x16U);
        const auto xd = wordFromDump(offset + 0x18U);
        const auto yd = wordFromDump(offset + 0x1AU);
        const auto link = static_cast<std::size_t>(linkWord) * 8U;
        const auto linkMode = static_cast<unsigned>((control >> 12U) & 0x3U);
        auto nextOffset = offset + 0x20U;
        switch (linkMode) {
        case 0U: break;
        case 1U: nextOffset = link; break;
        case 2U:
            if (returnAddress == static_cast<std::size_t>(-1)) {
                returnAddress = offset + 0x20U;
            }
            nextOffset = link;
            break;
        case 3U:
            if (returnAddress != static_cast<std::size_t>(-1)) {
                nextOffset = returnAddress;
                returnAddress = static_cast<std::size_t>(-1);
            }
            break;
        }
        output << "index=" << index << " offset=0x" << std::hex << offset
               << " control=0x" << control
               << " link=0x" << linkWord
               << " next=0x" << nextOffset
               << " action=" << action
               << " pmod=0x" << pmod
               << " color=0x" << color
               << " source_cell=0x" << sourceCell
               << " size=0x" << size
               << " source_address=0x" << (static_cast<unsigned>(sourceCell) * 8U)
               << " source_width_px=0x" << sourceWidth
               << " source_height_px=0x" << sourceHeight
               << " color_mode=0x" << colorMode
               << " xa=0x" << xa << " ya=0x" << ya
               << " xb=0x" << xb << " yb=0x" << yb
               << " xc=0x" << xc << " yc=0x" << yc
               << " xd=0x" << xd << " yd=0x" << yd
               << std::dec << '\n';
        offset = nextOffset;
        if (index == 1999U) ++malformedCount;
    }
    output << "# commands=" << commandCount << " skipped=" << skippedCount
           << " sprites=" << spriteCount << " polygons=" << polygonCount
           << " lines=" << lineCount << " state=" << stateCount
           << " malformed=" << malformedCount << '\n';
    return static_cast<bool>(output);
}

bool writeVdp1RegisterTrace(
    const std::filesystem::path& directory,
    const std::deque<saturn::platform::MmioAccess>& accesses) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    std::ofstream output(directory / "vdp1-register-writes.txt");
    if (!output) return false;
    std::size_t ordinal{};
    for (const auto& access : accesses) {
        if (access.space != saturn::platform::AddressSpace::Vdp1 || !access.write) continue;
        if ((access.address & 0x00F00000U) != 0x00D00000U) continue;
        const auto offset = access.address & 0x000FFFFFU;
        if (offset != 0x00000U && offset != 0x00002U && offset != 0x00004U) continue;
        output << ordinal++ << " step=" << access.context.step
               << " block=" << saturn::sh2::formatAddress(access.context.blockAddress)
               << " caller=" << saturn::sh2::formatAddress(access.context.callerBlock)
               << " address=" << saturn::sh2::formatAddress(access.address)
               << " reg=0x" << std::hex << offset
               << " value=0x" << access.value << std::dec << '\n';
    }
    return static_cast<bool>(output);
}

bool writeAudioWav(const std::filesystem::path& path, std::span<const float> samples,
                   std::uint32_t sampleRate, std::uint16_t channels) {
    if (channels == 0U || samples.size() % channels != 0U) return false;
    const auto frames = samples.size() / channels;
    const auto dataBytes = frames * channels * sizeof(std::int16_t);
    if (dataBytes > std::numeric_limits<std::uint32_t>::max()) return false;
    std::ofstream output(path, std::ios::binary);
    if (!output) return false;
    const auto writeLe16 = [&output](std::uint16_t value) {
        output.put(static_cast<char>(value));
        output.put(static_cast<char>(value >> 8U));
    };
    const auto writeLe32 = [&output](std::uint32_t value) {
        output.put(static_cast<char>(value));
        output.put(static_cast<char>(value >> 8U));
        output.put(static_cast<char>(value >> 16U));
        output.put(static_cast<char>(value >> 24U));
    };
    output.write("RIFF", 4);
    writeLe32(static_cast<std::uint32_t>(36U + dataBytes));
    output.write("WAVEfmt ", 8);
    writeLe32(16U);             // PCM fmt chunk size
    writeLe16(1U);              // PCM
    writeLe16(channels);
    writeLe32(sampleRate);
    writeLe32(sampleRate * channels * sizeof(std::int16_t));
    writeLe16(static_cast<std::uint16_t>(channels * sizeof(std::int16_t)));
    writeLe16(16U);
    output.write("data", 4);
    writeLe32(static_cast<std::uint32_t>(dataBytes));
    for (const auto sample : samples) {
        const auto clamped = std::clamp(sample, -1.0F, 1.0F);
        const auto pcm = static_cast<std::int16_t>(clamped <= -1.0F ? -32768.0F :
            clamped >= 1.0F ? 32767.0F : clamped * 32767.0F);
        writeLe16(static_cast<std::uint16_t>(pcm));
    }
    return static_cast<bool>(output);
}

struct FrontendQuit final {};
struct SlaveExecutionFailure final {
    saturn::core::Address address{};
    std::size_t step{};
};

// Small host frontend for the deterministic Saturn runtime. The emulator and
// the window/audio callbacks run on the same thread; waveOut only calls back
// to mark completed buffers, so no emulation state is touched from the audio
// callback thread.
class RealtimeFrontend final {
public:
    RealtimeFrontend() = default;
    RealtimeFrontend(const RealtimeFrontend&) = delete;
    RealtimeFrontend& operator=(const RealtimeFrontend&) = delete;

    ~RealtimeFrontend() { shutdown(); }

    [[nodiscard]] bool initialize(bool enableWindow, bool enableAudio) {
        windowEnabled_ = enableWindow;
        audioEnabled_ = enableAudio;
#if defined(_WIN32)
        if (windowEnabled_ && !openWindow()) {
            shutdown();
            return false;
        }
        if (audioEnabled_ && !openAudio()) {
            shutdown();
            return false;
        }
        // The default Windows timer can turn a 1 ms pacing sleep into an
        // entire display interval. Scope the finer resolution to this session.
        timerPeriodEnabled_ = timeBeginPeriod(1U) == TIMERR_NOERROR;
        nextFrame_ = Clock::now();
        fpsEpoch_ = nextFrame_;
        sessionStart_ = nextFrame_;
        return true;
#else
        static_cast<void>(enableWindow);
        static_cast<void>(enableAudio);
        return false;
#endif
    }

    void pump() noexcept {
#if defined(_WIN32)
        if (!windowEnabled_) return;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE) != 0) {
            if (message.message == WM_QUIT) {
                quit_ = true;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        input_.poll();
#endif
    }

    [[nodiscard]] bool shouldQuit() const noexcept { return quit_; }

    [[nodiscard]] bool takeDebugCaptureRequest() noexcept {
        return std::exchange(debugCaptureRequested_, false);
    }
    void setDebugEnabled(bool enabled) noexcept { debugEnabled_ = enabled; }

    [[nodiscard]] std::array<std::size_t, 6> debugCounters() {
#if defined(_WIN32)
        std::lock_guard lock(audioMutex_);
        return {audioUnderruns_, audioWaits_, audioWriteErrors_,
                audioTruncatedFrames_, nearBlackFrames_, videoFrames_};
#else
        return {};
#endif
    }

    void attachInput(saturn::platform::SmpcRuntime& smpc,
                     std::array<bool, 2> scriptedPorts, bool vulkan) {
#if defined(_WIN32)
        input_.attach(smpc, scriptedPorts);
        vulkan_ = vulkan;
        std::cout << "  controls: arrows; Z/X/C=A/B/C; A/S/D=X/Y/Z; Q/E=L/R; Enter=Start; F1=help\n";
#else
        static_cast<void>(smpc);
        static_cast<void>(scriptedPorts);
        static_cast<void>(vulkan);
#endif
    }

    void setInputScripted(std::size_t port, bool scripted) {
#if defined(_WIN32)
        input_.setScriptedPort(port, scripted);
#else
        static_cast<void>(port);
        static_cast<void>(scripted);
#endif
    }

    void onVideoFrame(const saturn::platform::VideoFrame& frame) {
#if defined(_WIN32)
        if (!windowEnabled_ || hwnd_ == nullptr || quit_) return;
        input_.advanceFrame();
        pump();
        if (quit_) return;
        // Wait before publishing the next image; waiting after publication
        // makes its interval depend on the next frame's CPU processing cost.
        paceFrame();
        if (quit_) return;
        const auto paintStart = Clock::now();
        frameWidth_ = frame.width;
        frameHeight_ = frame.height;
        framePixels_ = frame.pixels;
        // Count nearly black images produced by the renderer. Comparing this
        // counter with a visible flash distinguishes guest frames from the
        // window-paint path. Normal scene transitions can increment it too.
        if (!frame.pixels.empty()) {
            const auto threshold = std::max<std::size_t>(1U, frame.pixels.size() / 10U);
            std::size_t visible{};
            for (const auto pixel : frame.pixels) {
                if (((pixel >> 16U) & 0xFFU) >= 12U ||
                    ((pixel >> 8U) & 0xFFU) >= 12U || (pixel & 0xFFU) >= 12U) {
                    if (++visible >= threshold) break;
                }
            }
            if (visible < threshold) ++nearBlackFrames_;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
        ++fpsFrames_;
        ++videoFrames_;
        const auto now = Clock::now();
        paintTiming_.record(now - paintStart);
        if (lastPresentation_ != Clock::time_point{}) {
            frameTiming_.record(now - lastPresentation_);
        }
        lastPresentation_ = now;
        const auto elapsed = std::chrono::duration<double>(now - fpsEpoch_).count();
        if (elapsed >= 1.0) {
            std::wostringstream title;
            title << L"SaturnBox - Rockman X4 | " << std::fixed
                  << std::setprecision(1) << static_cast<double>(fpsFrames_) / elapsed << L" FPS | "
                  << (vulkan_ ? L"VDP1 Vulkan" : L"VDP1 CPU")
                  << L" | black frames " << nearBlackFrames_;
            if (audioEnabled_) {
                std::lock_guard lock(audioMutex_);
                title << L" | audio gaps " << audioUnderruns_
                      << L" clipped " << audioTruncatedFrames_;
            }
            title << L" | F1: controles";
            if (debugEnabled_) title << L" | F8: captura debug";
            SetWindowTextW(hwnd_, title.str().c_str());
            fpsEpoch_ = now;
            fpsFrames_ = 0U;
        }
#else
        static_cast<void>(frame);
#endif
    }

    void onAudioFrame(const saturn::platform::ScspRuntime::AudioFrame& frame) {
#if defined(_WIN32)
        if (!audioEnabled_ || waveOut_ == nullptr || frame.samples.empty()) return;
        std::unique_lock lock(audioMutex_);
        const auto hasFreeBuffer = [&] {
            return std::any_of(audioBuffers_.begin(), audioBuffers_.end(),
                [](const auto& buffer) { return !buffer.busy; });
        };
        // Backpressure preserves PCM instead of silently dropping a frame.
        while (!hasFreeBuffer()) {
            ++audioWaits_;
            audioReady_.wait_for(lock, std::chrono::milliseconds(10));
            lock.unlock();
            pump();
            lock.lock();
            if (quit_) return;
        }
        const auto queued = static_cast<std::size_t>(std::count_if(
            audioBuffers_.begin(), audioBuffers_.end(),
            [](const auto& buffer) { return buffer.busy; }));
        if (audioStarted_ && queued == 0U) {
            audioStarted_ = false;
            ++audioUnderruns_;
            lock.unlock();
            waveOutPause(waveOut_);
            lock.lock();
        }
        AudioBuffer* target = nullptr;
        for (auto& buffer : audioBuffers_) {
            if (!buffer.busy) {
                target = &buffer;
                break;
            }
        }
        if (target == nullptr) return;
        const auto count = std::min(frame.samples.size(), target->samples.size());
        if (count < frame.samples.size()) ++audioTruncatedFrames_;
        for (std::size_t index = 0; index < count; ++index) {
            const auto sample = std::clamp(frame.samples[index], -1.0F, 1.0F);
            target->samples[index] = static_cast<std::int16_t>(
                sample <= -1.0F ? -32768.0F :
                sample >= 1.0F ? 32767.0F : sample * 32767.0F);
        }
        target->header.dwBufferLength = static_cast<DWORD>(
            count * sizeof(std::int16_t));
        target->busy = true;
        // WinMM can deliver WOM_DONE while an API call is still in progress.
        // Never hold the callback's mutex across a WinMM call.
        lock.unlock();
        const auto written = waveOutWrite(waveOut_, &target->header, sizeof(WAVEHDR));
        lock.lock();
        if (written != MMSYSERR_NOERROR) {
            target->busy = false;
            ++audioWriteErrors_;
            return;
        }
        // Prime the device with three blocks once. After an underrun, resume on the
        // first available frame instead of inserting another 50 ms silence
        // gap while waiting to rebuild the initial reserve.
        const auto requiredPrebuffer = audioHasStarted_ ? 1U : kAudioPrebufferCount;
        if (!audioStarted_ && queued + 1U >= requiredPrebuffer) {
            lock.unlock();
            const auto restarted = waveOutRestart(waveOut_);
            lock.lock();
            if (restarted == MMSYSERR_NOERROR) {
                audioStarted_ = true;
                audioHasStarted_ = true;
                nextFrame_ = Clock::now();
            }
        }
#else
        static_cast<void>(frame);
#endif
    }

    void printStats() const {
#if defined(_WIN32)
        std::cout << "  frontend frames: " << videoFrames_
                  << ", average FPS: " << (static_cast<double>(videoFrames_) /
                      std::max(0.001, std::chrono::duration<double>(Clock::now() - sessionStart_).count()))
                  << ", nearly black frames: " << nearBlackFrames_
                  << ", audio underruns: " << audioUnderruns_
                  << ", audio waits: " << audioWaits_
                  << ", audio write errors: " << audioWriteErrors_
                  << ", audio truncated blocks: " << audioTruncatedFrames_
                  << ", audio refill frames: " << audioRefillFrames_
                  << ", audio catch-up frames: " << audioCatchupFrames_
                  << ", 1 ms timer: " << (timerPeriodEnabled_ ? "yes" : "no") << '\n';
        frameTiming_.print("frame interval");
        paintTiming_.print("window paint");
        sleepOvershoot_.print("pacing sleep overshoot");
#endif
    }

private:
#if defined(_WIN32)
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t kAudioBufferCount = 8U;
    static constexpr std::size_t kAudioPrebufferCount = 3U;
    // A late frame can contain several thousand samples at the real-time
    // audio rate. Keep 500 ms of room in each prepared buffer.
    static constexpr std::size_t kMaxAudioSamplesPerBlock = 22050U;
    static constexpr std::size_t kAudioBufferSamples = kMaxAudioSamplesPerBlock * 2U;
    static constexpr auto kFramePeriod = std::chrono::microseconds(16667);

    struct TimingStats {
        std::array<std::size_t, 256U> milliseconds{};
        std::size_t count{};
        double totalMs{}, maximumMs{};

        void record(Clock::duration elapsed) noexcept {
            const auto ms = std::max(0.0, std::chrono::duration<double, std::milli>(elapsed).count());
            ++milliseconds[static_cast<std::size_t>(std::min(ms, 255.0))];
            ++count;
            totalMs += ms;
            maximumMs = std::max(maximumMs, ms);
        }

        [[nodiscard]] double percentile(std::size_t percent) const noexcept {
            if (count == 0U) return 0.0;
            const auto target = (count * percent + 99U) / 100U;
            std::size_t cumulative = 0U;
            for (std::size_t index = 0U; index < milliseconds.size(); ++index) {
                cumulative += milliseconds[index];
                if (cumulative >= target) {
                    return index + 1U == milliseconds.size() ? maximumMs
                        : static_cast<double>(index + 1U);
                }
            }
            return maximumMs;
        }

        void print(const char* label) const {
            std::cout << "  " << label << " ms: mean=" << (count == 0U ? 0.0 : totalMs / count)
                      << ", p95 bucket upper=" << percentile(95U)
                      << ", p99 bucket upper=" << percentile(99U)
                      << ", max=" << maximumMs << ", samples=" << count << '\n';
        }
    };

    struct AudioBuffer {
        std::vector<std::int16_t> samples;
        WAVEHDR header{};
        bool prepared{};
        bool busy{};
    };

    [[nodiscard]] bool openWindow() {
        constexpr wchar_t kClassName[] = L"SaturnBoxRuntimeWindow";
        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSW windowClass{};
        windowClass.hInstance = instance;
        windowClass.lpfnWndProc = &RealtimeFrontend::windowProc;
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        windowClass.lpszClassName = kClassName;
        if (RegisterClassW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        RECT rectangle{0, 0, 960, 672};
        if (AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE) == 0) return false;
        hwnd_ = CreateWindowExW(0, kClassName, L"SaturnBox - Rockman X4",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                rectangle.right - rectangle.left,
                                rectangle.bottom - rectangle.top,
                                nullptr, nullptr, instance, this);
        if (hwnd_ == nullptr) return false;
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        return true;
    }

    [[nodiscard]] bool openAudio() {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = static_cast<WORD>(saturn::platform::ScspRuntime::kAudioChannels);
        format.nSamplesPerSec = saturn::platform::ScspRuntime::kAudioSampleRate;
        format.wBitsPerSample = 16U;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * sizeof(std::int16_t));
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        if (waveOutOpen(&waveOut_, WAVE_MAPPER, &format,
                        reinterpret_cast<DWORD_PTR>(&RealtimeFrontend::waveCallback),
                        reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
            waveOut_ = nullptr;
            return false;
        }
        if (waveOutPause(waveOut_) != MMSYSERR_NOERROR) {
            closeAudio();
            return false;
        }
        for (auto& buffer : audioBuffers_) {
            buffer.samples.resize(kAudioBufferSamples);
            buffer.header.lpData = reinterpret_cast<LPSTR>(buffer.samples.data());
            buffer.header.dwBufferLength = static_cast<DWORD>(
                buffer.samples.size() * sizeof(std::int16_t));
            if (waveOutPrepareHeader(waveOut_, &buffer.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                closeAudio();
                return false;
            }
            buffer.prepared = true;
        }
        return true;
    }

    void closeAudio() noexcept {
        if (waveOut_ == nullptr) return;
        waveOutReset(waveOut_);
        for (auto& buffer : audioBuffers_) {
            if (buffer.prepared) {
                waveOutUnprepareHeader(waveOut_, &buffer.header, sizeof(WAVEHDR));
                buffer.prepared = false;
            }
            buffer.busy = false;
        }
        waveOutClose(waveOut_);
        waveOut_ = nullptr;
    }

    void shutdown() noexcept {
        closeAudio();
        if (hwnd_ != nullptr) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        releaseBackBuffer();
        if (timerPeriodEnabled_) {
            timeEndPeriod(1U);
            timerPeriodEnabled_ = false;
        }
    }

    void paceFrame() {
        auto now = Clock::now();
        // Refill the initial audio reserve before imposing a display wait.
        // This also lets the producer recover after a late frame while the
        // device is still playing; no PCM is dropped, repeated or resampled.
        if (audioEnabled_) {
            std::lock_guard lock(audioMutex_);
            const auto queued = static_cast<std::size_t>(std::count_if(
                audioBuffers_.begin(), audioBuffers_.end(),
                [](const auto& buffer) { return buffer.busy; }));
            if (!audioStarted_ || queued < kAudioPrebufferCount) {
                if (audioStarted_) ++audioCatchupFrames_;
                else ++audioRefillFrames_;
                nextFrame_ = now;
                return;
            }
        }
        if (nextFrame_ == Clock::time_point{}) nextFrame_ = now;
        while (!quit_ && now < nextFrame_) {
            pump();
            if (quit_) return;
            const auto remaining = nextFrame_ - now;
            const auto remainingMicros = std::chrono::duration_cast<
                std::chrono::microseconds>(remaining);
            const auto requested = std::min(remainingMicros, std::chrono::microseconds(1000));
            const auto sleepStart = Clock::now();
            std::this_thread::sleep_for(requested);
            now = Clock::now();
            sleepOvershoot_.record(now - sleepStart - requested);
        }
        nextFrame_ += kFramePeriod;
        if (now > nextFrame_ + kFramePeriod * 4) nextFrame_ = now + kFramePeriod;
    }

    void paint(HDC deviceContext) {
        RECT client{};
        GetClientRect(hwnd_, &client);
        const auto width = client.right - client.left;
        const auto height = client.bottom - client.top;
        if (width <= 0 || height <= 0 || framePixels_.empty() ||
            frameWidth_ == 0U || frameHeight_ == 0U) return;
        // Keep sprites proportional when the window or TVMD height changes.
        auto drawWidth = width;
        auto drawHeight = static_cast<LONG>(
            static_cast<std::int64_t>(width) * frameHeight_ / frameWidth_);
        if (drawHeight > height) {
            drawHeight = height;
            drawWidth = static_cast<LONG>(
                static_cast<std::int64_t>(height) * frameWidth_ / frameHeight_);
        }
        const auto buffered = ensureBackBuffer(deviceContext, width, height);
        const auto paintContext = buffered ? backBufferDc_ : deviceContext;
        FillRect(paintContext, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(frameWidth_);
        bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(frameHeight_);
        bitmapInfo.bmiHeader.biPlanes = 1U;
        bitmapInfo.bmiHeader.biBitCount = 32U;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(paintContext, (width - drawWidth) / 2, (height - drawHeight) / 2,
                      drawWidth, drawHeight, 0, 0,
                      static_cast<int>(frameWidth_), static_cast<int>(frameHeight_),
                      framePixels_.data(), &bitmapInfo, DIB_RGB_COLORS, SRCCOPY);
        if (helpVisible_) {
            RECT panel{12, 12, std::min(width - 12, 660L), 166};
            FillRect(paintContext, &panel, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SetTextColor(paintContext, RGB(255, 255, 255));
            SetBkMode(paintContext, TRANSPARENT);
            panel.left += 8; panel.top += 8;
            DrawTextW(paintContext,
                L"Controles Saturn - F1 fecha esta ajuda\n"
                L"Setas: mover | Enter: Start\n"
                L"Z / X / C: A / B / C | A / S / D: X / Y / Z | Q / E: L / R\n"
                L"Gamepad: direcional/analogico esquerdo; Start\n"
                L"Xbox X / A / B: Saturn A / B / C; LB / Y / RB: X / Y / Z\n"
                L"Gatilhos LT / RT: Saturn L / R; segundo gamepad: porta 2",
                -1, &panel, DT_LEFT | DT_TOP | DT_NOPREFIX);
        }
        if (buffered) BitBlt(deviceContext, 0, 0, width, height, backBufferDc_, 0, 0, SRCCOPY);
    }

    [[nodiscard]] bool ensureBackBuffer(HDC deviceContext, int width, int height) noexcept {
        if (backBufferDc_ != nullptr && backBufferBitmap_ != nullptr &&
            backBufferWidth_ == width && backBufferHeight_ == height) return true;
        releaseBackBuffer();
        const auto memoryDc = CreateCompatibleDC(deviceContext);
        if (memoryDc == nullptr) return false;
        const auto bitmap = CreateCompatibleBitmap(deviceContext, width, height);
        if (bitmap == nullptr) {
            DeleteDC(memoryDc);
            return false;
        }
        const auto previousBitmap = SelectObject(memoryDc, bitmap);
        if (previousBitmap == nullptr || previousBitmap == HGDI_ERROR) {
            DeleteObject(bitmap);
            DeleteDC(memoryDc);
            return false;
        }
        backBufferDc_ = memoryDc;
        backBufferBitmap_ = bitmap;
        backBufferPreviousBitmap_ = previousBitmap;
        backBufferWidth_ = width;
        backBufferHeight_ = height;
        return true;
    }

    void releaseBackBuffer() noexcept {
        if (backBufferDc_ != nullptr && backBufferPreviousBitmap_ != nullptr) {
            SelectObject(backBufferDc_, backBufferPreviousBitmap_);
        }
        if (backBufferBitmap_ != nullptr) DeleteObject(backBufferBitmap_);
        if (backBufferDc_ != nullptr) DeleteDC(backBufferDc_);
        backBufferDc_ = nullptr;
        backBufferBitmap_ = nullptr;
        backBufferPreviousBitmap_ = nullptr;
        backBufferWidth_ = 0;
        backBufferHeight_ = 0;
    }

    static void CALLBACK waveCallback(HWAVEOUT, UINT message, DWORD_PTR instance,
                                      DWORD_PTR parameter1, DWORD_PTR) {
        if (message != WOM_DONE || instance == 0U) return;
        auto* frontend = reinterpret_cast<RealtimeFrontend*>(instance);
        auto* completed = reinterpret_cast<WAVEHDR*>(parameter1);
        std::lock_guard lock(frontend->audioMutex_);
        for (auto& buffer : frontend->audioBuffers_) {
            if (&buffer.header == completed) {
                buffer.busy = false;
                frontend->audioReady_.notify_one();
                break;
            }
        }
    }

    static LRESULT CALLBACK windowProc(HWND window, UINT message,
                                       WPARAM wParam, LPARAM lParam) {
        auto* frontend = reinterpret_cast<RealtimeFrontend*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            frontend = static_cast<RealtimeFrontend*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(frontend));
            frontend->hwnd_ = window;
        }
        if (frontend != nullptr) {
            switch (message) {
            case WM_SETFOCUS:
                frontend->input_.setFocus(true);
                return 0;
            case WM_KILLFOCUS:
                frontend->input_.setFocus(false);
                return 0;
            case WM_KEYDOWN:
            case WM_KEYUP:
                frontend->input_.setKey(static_cast<std::size_t>(wParam), message == WM_KEYDOWN);
                if (message == WM_KEYDOWN && wParam == VK_F1 && (lParam & (1LL << 30U)) == 0) {
                    frontend->helpVisible_ = !frontend->helpVisible_;
                    InvalidateRect(window, nullptr, FALSE);
                }
                if (frontend->debugEnabled_ && message == WM_KEYDOWN && wParam == VK_F8 &&
                    (lParam & (1LL << 30U)) == 0)
                    frontend->debugCaptureRequested_ = true;
                return 0;
            case WM_PAINT: {
                PAINTSTRUCT paintStruct{};
                const auto deviceContext = BeginPaint(window, &paintStruct);
                frontend->paint(deviceContext);
                EndPaint(window, &paintStruct);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_CLOSE:
                DestroyWindow(window);
                return 0;
            case WM_DESTROY:
                frontend->quit_ = true;
                PostQuitMessage(0);
                return 0;
            default:
                break;
            }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    HWND hwnd_{};
    HWAVEOUT waveOut_{};
    HDC backBufferDc_{};
    HBITMAP backBufferBitmap_{};
    HGDIOBJ backBufferPreviousBitmap_{};
    int backBufferWidth_{}, backBufferHeight_{};
    std::array<AudioBuffer, kAudioBufferCount> audioBuffers_{};
    std::mutex audioMutex_;
    std::condition_variable audioReady_;
    bool audioStarted_{}, audioHasStarted_{};
    saturn::host::Win32Input input_;
    bool helpVisible_{}, vulkan_{};
    bool timerPeriodEnabled_{};
    std::size_t audioRefillFrames_{};
    std::size_t audioCatchupFrames_{};
    std::size_t audioUnderruns_{}, audioWaits_{}, audioWriteErrors_{}, audioTruncatedFrames_{};
    std::size_t fpsFrames_{}, videoFrames_{}, nearBlackFrames_{};
    Clock::time_point fpsEpoch_{};
    Clock::time_point sessionStart_{};
    Clock::time_point nextFrame_{};
    Clock::time_point lastPresentation_{};
    TimingStats frameTiming_{}, paintTiming_{}, sleepOvershoot_{};
#endif
    bool windowEnabled_{};
    bool audioEnabled_{};
    bool quit_{};
    bool debugCaptureRequested_{};
    bool debugEnabled_{};
#if defined(_WIN32)
    std::uint32_t frameWidth_{320U};
    std::uint32_t frameHeight_{224U};
    std::vector<std::uint32_t> framePixels_ =
        std::vector<std::uint32_t>(320U * 224U, 0xFF000000U);
#endif
};

int executeMap(const saturn::core::MemoryMap& image, saturn::core::Address entry,
        std::size_t maxSteps, bool forceInterpreter) {
    const auto graph = saturn::sh2::BlockBuilder{}.build(image, std::vector{entry});
    saturn::sir::Module module;
    const auto liftReport = saturn::sh2::lowerCFG(graph, module);
    if (!liftReport.success()) {
        std::cerr << "error: unable to lower entry CFG; failures="
                  << liftReport.failures.size() << '\n';
        return 1;
    }

    saturn::sir::MachineState state;
    loadState(image, state);
    // Saturn's IP header leaves STACK-M zero for this disc. The boot ROM
    // default places the master stack immediately below the IP and
    // initializes the vector base in low work RAM.
    state.writeState(saturn::sh2::stateSlot(saturn::sh2::Register::R15),
                     saturn::platform::BootRomServices::kDefaultMasterStack);
    state.writeState(saturn::sh2::stateSlot(saturn::sh2::Register::VBR),
                     saturn::platform::BootRomServices::kDefaultVectorBase);
    saturn::platform::MmioInterceptor mmio;
    mmio.attachMachineState(state);
    state.setMemoryInterceptor(&mmio);

    saturn::sh2::RuntimeBlockResolver resolver(image, module);
    saturn::sir::Win64JitBlockExecutor nativeJit;
    std::size_t staticBlockCount{};
    const auto* staticBlocks = builtInStaticBlocks(staticBlockCount);
    saturn::sir::StaticNativeBlockExecutor staticNative(
        module, std::span<const saturn::sir::StaticNativeBlockEntry>{
                    staticBlocks, staticBlockCount});
    std::size_t interpreterFallbackBlocks{};
    saturn::sir::ProgramExecutionOptions options;
    options.maxSteps = maxSteps;
    if (!forceInterpreter && (staticBlockCount > 0U ||
                              saturn::sir::Win64JitBlockExecutor::available())) {
        options.executeBlock = [&staticNative, &nativeJit, &interpreterFallbackBlocks](const saturn::sir::BasicBlock& block,
                                            saturn::sir::MachineState& machineState) {
            if (auto result = staticNative.execute(block, machineState)) return result;
            auto result = nativeJit.execute(block, machineState);
            if (!result) ++interpreterFallbackBlocks;
            return result;
        };
        if (staticBlockCount == 0U) {
            options.executeChain = [&nativeJit](const saturn::sir::BasicBlock& block,
                                                saturn::sir::MachineState& machineState,
                                                std::size_t maxBlocks,
                                                const saturn::sir::BlockChainContinuation& continuation) {
                return nativeJit.executeChain(block, machineState, maxBlocks, continuation);
            };
        }
    }
    options.resolveBlock = [&resolver](saturn::core::Address address) {
        return resolver.resolve(address);
    };
    const auto execution = saturn::sir::ProgramExecutor{}.run(module.blockAt(entry), state, options);
    const auto& resolution = resolver.stats();

    std::cout << "SIR execution\n"
              << "  entry: " << saturn::sh2::formatAddress(entry) << '\n'
              << "  status: " << programStatusName(execution.status) << '\n'
              << "  completed block steps: " << execution.steps << '\n'
              << "  last block: " << saturn::sh2::formatAddress(execution.address) << '\n'
              << "  dynamic target requests: " << resolution.requests << '\n'
              << "  reused dynamic blocks: " << resolution.reusedBlocks << '\n'
              << "  blocks lowered on demand: " << resolution.loweredBlocks << '\n'
              << "  resolver block re-lowerings: " << resolution.replacedBlocks << '\n'
              << "  static native blocks compiled: " << staticNative.blockCount() << '\n'
              << "  static native blocks executed: " << staticNative.executedBlockCount() << '\n'
              << "  native JIT blocks compiled: " << nativeJit.compiledBlockCount() << '\n'
              << "  native JIT blocks executed: " << nativeJit.executedBlockCount() << '\n'
              << "  native JIT chained transfers: " << nativeJit.chainedTransferCount() << '\n'
              << "  interpreter fallback blocks: " << interpreterFallbackBlocks << '\n'
              << "  dynamic resolution failures: " << resolution.failures << '\n'
              << "  MMIO accesses: " << mmio.accessCount() << " (retained "
              << mmio.accesses().size() << ")\n";
    if (execution.status == saturn::sir::ProgramStatus::UnresolvedTarget) {
        std::cout << "  unresolved target: "
                  << saturn::sh2::formatAddress(execution.unresolvedTarget) << '\n';
    }
    if (execution.status == saturn::sir::ProgramStatus::Trap) {
        std::cout << "  trap vector: " << static_cast<unsigned>(execution.last.trapVector) << '\n';
    }
    return execution.status == saturn::sir::ProgramStatus::InterpreterFailure ? 1 : 0;
}

int run(const saturn::core::MemoryMap& image, saturn::core::Address entry,
        std::size_t maxSteps, bool forceInterpreter) {
    return executeMap(image, entry, maxSteps, forceInterpreter);
}

int boot(const Arguments& arguments) {
    if (arguments.resourcePath.has_value() != arguments.resourceBase.has_value()) {
        std::cerr << "error: --resource and --resource-base must be supplied together\n";
        return 2;
    }
    if (arguments.resourcePath && !arguments.discImage) {
        std::cerr << "error: --resource requires --disc\n";
        return 2;
    }
    const auto program = saturn::core::MemoryMap::fromFile(
        *arguments.program, arguments.programBase);
    const auto aip = arguments.ipImage
        ? std::optional<saturn::core::MemoryMap>{}
        : saturn::core::MemoryMap::fromFile(arguments.input, arguments.aipBase);
    const auto ip = arguments.ipImage
        ? saturn::core::MemoryMap::fromFile(*arguments.ipImage, arguments.ipBase)
        : std::optional<saturn::core::MemoryMap>{};
    if ((!aip && !ip) || !program) {
        std::cerr << "error: unable to load AIP or program image\n";
        return 1;
    }

    saturn::core::MemoryMap image;
    const auto& bootImage = ip ? *ip : *aip;
    for (const auto& [_, segment] : bootImage.segments()) {
        if (!image.addSegment(segment.base, segment.bytes)) {
            std::cerr << "error: overlapping or invalid boot image segment\n";
            return 1;
        }
    }
    for (const auto& [_, segment] : program->segments()) {
        if (!image.addSegment(segment.base, segment.bytes)) {
            std::cerr << "error: overlapping or invalid program segment\n";
            return 1;
        }
    }

    const auto entry = arguments.entry.value_or(
        arguments.ipImage ? static_cast<saturn::core::Address>(arguments.ipBase + 0xE40U)
                          : arguments.aipBase);
    const auto expectedEntry = arguments.expectedEntry.value_or(arguments.programBase);
    if (!image.contains(entry, 2)) {
        std::cerr << "error: AIP entry " << saturn::sh2::formatAddress(entry)
                  << " is not present in the boot image\n";
        return 1;
    }
    const auto graph = saturn::sh2::BlockBuilder{}.build(image, std::vector{entry});
    saturn::sir::Module module;
    const auto liftReport = saturn::sh2::lowerCFG(graph, module);
    if (!liftReport.success()) {
        std::cerr << "error: unable to lower AIP CFG; failures="
                  << liftReport.failures.size() << '\n';
        return 1;
    }

    saturn::sir::MachineState state;
    loadState(image, state);
    saturn::platform::MmioInterceptor mmio;
    mmio.setAccessRecording(arguments.trace ||
        (arguments.frameDirectory.has_value() && !arguments.traceGame &&
         !arguments.frameCaptureAfterBlocks));
    if (arguments.scspDriver) {
        try {
            if (!mmio.scsp().enableDriverCpu()) {
                std::cerr << "error: --scsp-driver requires a build with SATURNBOX_MUSASHI_68K=ON\n";
                return 1;
            }
        } catch (const std::exception& error) {
            std::cerr << "error: SCSP driver: " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << "  SCSP mode: " << (arguments.scspDriver ? "68000 + slots" : "PCM HLE") << '\n';
    mmio.setControlTraceEnabled(arguments.controlTraceOutput.has_value());
    mmio.setControlTraceStartStep(arguments.controlTraceFromStep.value_or(0U));
    mmio.setControlTraceOreg2Only(arguments.controlTraceOreg2Only);
    mmio.attachMachineState(state);
    mmio.video().setDeferredComposition(!arguments.trace &&
        !arguments.traceGame && (!arguments.frameDirectory || arguments.frameCaptureAfterBlocks) &&
        !arguments.blackFrameProbe &&
        !arguments.vdp2MapTrace && !arguments.vdp2MapReadTrace);
    if (arguments.frameCaptureAfterBlocks) {
        mmio.video().setDiagnosticMemoryCapture(true);
        mmio.video().setDiagnosticCaptureEnabled(false);
        mmio.video().setVdp1CommandTraceEnabled(true);
        mmio.video().setVdp1MemoryWriteTraceEnabled(true);
    }
    if (arguments.vdp2MapReadTrace) mmio.video().setMapReadTrace(true);
    if (arguments.discImage) {
        auto sectorSize = arguments.discSectorSize;
        // MDF/raw tracks conventionally store 2448-byte sectors (2352 bytes
        // plus subchannel); 2352-byte raw tracks are also common. ISO/IMG
        // data remains the default 2048-byte layout unless overridden.
        std::error_code discError;
        const auto discSize = std::filesystem::file_size(*arguments.discImage, discError);
        if (discError || discSize == 0U) {
            std::cerr << "error: unable to load disc image "
                      << arguments.discImage->string() << '\n';
            return 1;
        }
        if (sectorSize == 2048U && discSize % 2048U != 0U &&
            discSize % 2448U == 0U) {
            sectorSize = 2448U;
        } else if (sectorSize == 2048U && discSize % 2048U != 0U &&
                   discSize % 2352U == 0U) {
            sectorSize = 2352U;
        }
        if (!mmio.cd().setImageFile(*arguments.discImage, sectorSize)) {
            std::cerr << "error: disc image size is not a multiple of "
                      << sectorSize << " bytes\n";
            return 1;
        }
    }
    if (arguments.traceCdSectors) mmio.cd().setSectorTraceEnabled(true);
    std::vector<std::pair<std::size_t, saturn::platform::DigitalPadState>> padSequenceEvents;
    if (arguments.padSequence) {
        const auto parsed = parsePadSequence(*arguments.padSequence);
        if (!parsed) {
            std::cerr << "error: invalid --pad-sequence; use increasing block:buttons entries"
                         " separated by ';', for example"
                         " 61000000:start;61100000:release;64000000:right\n";
            return 2;
        }
        padSequenceEvents = *parsed;
    }
    std::ofstream x4PlayerSamplesFile;
    std::size_t x4PlayerSampleCount{};
    bool x4PlayerSampleWriteFailure{};
    if (arguments.x4PlayerSamplesOutput) {
        std::error_code sampleError;
        if (!arguments.x4PlayerSamplesOutput->parent_path().empty()) {
            std::filesystem::create_directories(
                arguments.x4PlayerSamplesOutput->parent_path(), sampleError);
        }
        if (sampleError) {
            std::cerr << "error: unable to create X4 player sample directory: "
                      << sampleError.message() << '\n';
            return 1;
        }
        x4PlayerSamplesFile.open(*arguments.x4PlayerSamplesOutput, std::ios::trunc);
        if (!x4PlayerSamplesFile) {
            std::cerr << "error: unable to write X4 player samples "
                      << arguments.x4PlayerSamplesOutput->string() << '\n';
            return 1;
        }
        x4PlayerSamplesFile << "# phase=before_pad_change; event_index=zero_based\n"
            << "event_index,scheduled_step,actual_step,block,frame_sequence,"
               "pad_before,pad_next,x_raw,vx_raw,held_raw,previous_raw,pressed_raw,"
               "gate_84,gate_89,gate_c3\n";
    }
    if (arguments.padLiveAfterBlocks &&
        (padSequenceEvents.empty() ||
         *arguments.padLiveAfterBlocks < padSequenceEvents.back().first)) {
        std::cerr << "error: --pad-live-after-blocks must be at or after the last pad-sequence event\n";
        return 2;
    }
    std::array<std::optional<saturn::platform::DigitalPadState>, 2> delayedPads;
    std::array<std::optional<saturn::platform::DigitalPadState>, 2> configuredPadStates;
    std::array<bool, 2> configuredPads{};
    for (std::size_t port = 0; port < arguments.padButtons.size(); ++port) {
        if (!arguments.padButtons[port]) {
            continue;
        }
        configuredPads[port] = true;
        const auto pad = parsePadState(*arguments.padButtons[port]);
        if (!pad) {
            std::cerr << "error: invalid --pad" << (port + 1U)
                      << " value; use comma-separated up,down,left,right,start,a,b,c,x,y,z,l,r"
                      << " or none\n";
            return 2;
        }
        configuredPadStates[port] = *pad;
        if (arguments.padAfterBlocks == 0U) {
            mmio.smpc().setPad(port, *pad);
            mmio.recordPadInput(0U, port, *pad);
        } else {
            delayedPads[port] = *pad;
        }
    }
    std::size_t presentedFrames{};
    bool frameWriteFailure{};
    bool frameCaptureArmed{};
    bool frameCaptured{};
    std::size_t frameCaptureArmedAt{};
    std::vector<float> capturedAudio;
    std::optional<RealtimeFrontend> realtime;
    if (arguments.window || arguments.audioLive) {
        realtime.emplace();
        if (!realtime->initialize(arguments.window, arguments.audioLive)) {
            std::cerr << "error: realtime frontend is unavailable or could not be initialized\n";
            return 1;
        }
    }
    std::shared_ptr<saturn::platform::Vdp1ComputeRenderer> vdp1Renderer;
    if (arguments.renderer != "cpu") {
        std::string error;
        vdp1Renderer = saturn::platform::createVulkanVdp1(error);
        if (!vdp1Renderer && arguments.renderer == "vulkan") {
            std::cerr << "error: Vulkan VDP1: " << error << '\n';
            return 1;
        }
        if (!vdp1Renderer) std::cout << "  Vulkan unavailable, using CPU VDP1: " << error << '\n';
        mmio.video().setVdp1Renderer(vdp1Renderer);
    }
    std::cout << "  VDP1 renderer: " << (vdp1Renderer ? "Vulkan compute / " : "CPU reference")
              << (vdp1Renderer ? vdp1Renderer->deviceName() : std::string_view{}) << std::endl;
    if (realtime && arguments.window) {
        realtime->attachInput(mmio.smpc(),
            {arguments.padButtons[0].has_value() || arguments.padSequence.has_value(),
             arguments.padButtons[1].has_value()},
            static_cast<bool>(vdp1Renderer));
    }
    const auto realtimeWindow = realtime && arguments.window ? &*realtime : nullptr;
    const auto realtimeAudio = realtime && arguments.audioLive ? &*realtime : nullptr;
    std::unique_ptr<saturn::tools::LiveDebugSession> liveDebug;
    const auto debugStart = std::chrono::steady_clock::now();
    auto nextDebugPoll = debugStart;
    bool debugAttachNextFrame{};
    std::string debugReportedError;
    if (arguments.debugSessionDirectory) {
        try {
            liveDebug = std::make_unique<saturn::tools::LiveDebugSession>(*arguments.debugSessionDirectory);
        } catch (const std::exception& error) {
            std::cerr << "error: live debugger: " << error.what() << '\n';
            return 1;
        }
        if (liveDebug->hasError()) {
            std::cerr << "error: live debugger: " << liveDebug->lastError() << '\n';
            return 1;
        }
        mmio.video().setVdp1CommandTraceEnabled(true);
        mmio.video().setVdp1MemoryWriteTraceEnabled(true);
        if (realtimeWindow) realtimeWindow->setDebugEnabled(true);
        std::ofstream latest(*arguments.debugSessionDirectory / "latest-session.txt");
        latest << liveDebug->directory().string() << '\n';
        if (!latest) std::cerr << "warning: cannot write latest debugger session pointer\n";
        std::ofstream config(liveDebug->directory() / "runtime.txt");
        config << "compiler_build_stamp=" << __DATE__ << ' ' << __TIME__ << '\n'
               << "renderer=" << (vdp1Renderer ? "vulkan" : "cpu") << '\n'
               << "vblank_blocks=" << arguments.vblankPeriodBlocks << '\n'
               << "scsp_driver=" << arguments.scspDriver << '\n'
               << "aip=" << arguments.input.string() << '\n'
               << "program=" << (arguments.program ? arguments.program->string() : "") << '\n'
               << "disc=" << (arguments.discImage ? arguments.discImage->string() : "") << '\n'
               << "disc_sector_size=" << arguments.discSectorSize << '\n'
               << "pad_sequence=" << arguments.padSequence.value_or("") << '\n';
        if (!config) std::cerr << "warning: cannot write debugger runtime configuration\n";
        std::cout << "  live debugger: " << liveDebug->directory().string()
                  << " (F8 or capture.request)\n";
    }
    const auto debugMetadata = [&]() {
        saturn::tools::DebugMetadata metadata;
        metadata.utcEpochMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        metadata.elapsedMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - debugStart).count());
        metadata.pad = mmio.smpc().pad(0).pressed;
        for (unsigned byte = 0; byte < 4U; ++byte)
            metadata.playerXRaw = (metadata.playerXRaw << 8U) | state.readRaw8(0x060545A8U + byte);
        metadata.playerX = metadata.playerXRaw >> 16U;
        const auto& sound = mmio.scsp().stats();
        std::ostringstream stats;
        stats << "frames=" << sound.audioFrames << ";samples=" << sound.audioSamples
              << ";voices=" << sound.activeVoices << ";pcm_starts=" << sound.pcmStarts
              << ";driver_cycles=" << sound.driverCycles << ";driver_pc=" << sound.driverPc;
        stats << ";slot_key_ons=" << sound.slotKeyOns << ";slot_mask=" << std::hex
              << sound.slotMask << std::dec << ";noise_samples=" << sound.noiseSamples
              << ";plfo_samples=" << sound.pitchLfoSamples << ";alfo_samples=" << sound.amplitudeLfoSamples
              << ";fm_samples=" << sound.fmSamples << ";dsp_send=" << sound.dspSendSamples
              << ";dsp_effect=" << sound.dspEffectSamples << ";audio_blocks=" << sound.audioBlockTicks
              << ";audio_ahead=" << sound.audioSubFrameSamples;
        for (const auto command : {0x01U, 0x05U, 0x08U, 0x0EU, 0x85U, 0x86U, 0x87U, 0x8AU})
            stats << ";cmd_" << std::hex << command << '=' << std::dec << sound.soundCommands[command];
        if (realtime) {
            const auto counters = realtime->debugCounters();
            metadata.audioUnderruns = counters[0];
            stats << ";audio_waits=" << counters[1] << ";audio_errors=" << counters[2]
                  << ";audio_truncated=" << counters[3] << ";dark_frames=" << counters[4]
                  << ";frontend_frames=" << counters[5];
        }
        metadata.scspStats = stats.str();
        return metadata;
    };
    if (liveDebug) mmio.video().setDebugEventSink([&](std::string_view phase) {
        if (!liveDebug->isCapturing()) return;
        auto snapshot = mmio.video().captureDebugSnapshot(true);
        auto metadata = debugMetadata();
        metadata.step = snapshot.contextStep;
        metadata.block = snapshot.contextBlock;
        metadata.caller = snapshot.contextCaller;
        liveDebug->recordPhase(phase, std::move(snapshot), std::move(metadata));
    });
    if (arguments.frameDirectory || realtimeWindow != nullptr || liveDebug) {
        const auto directory = arguments.frameDirectory ? *arguments.frameDirectory
                                                         : std::filesystem::path{};
        std::error_code error;
        if (arguments.frameDirectory) std::filesystem::create_directories(directory, error);
        if (arguments.frameDirectory && error) {
            std::cerr << "error: unable to create frame directory "
                      << directory.string() << ": " << error.message() << '\n';
            return 1;
        }
        mmio.video().setFrameSink([directory, realtimeWindow, arguments,
                                   &liveDebug, &debugMetadata, &debugAttachNextFrame,
                                   &nextDebugPoll, &state, &mmio,
                                   &debugReportedError,
                                   &presentedFrames, &frameWriteFailure,
                                   &frameCaptureArmed, &frameCaptured,
                                   &frameCaptureArmedAt,
                                   previous = saturn::platform::VideoFrame{}](
                                      const saturn::platform::VideoFrame& frame) mutable {
            if (realtimeWindow != nullptr) realtimeWindow->onVideoFrame(frame);
            if (liveDebug) {
                const auto now = std::chrono::steady_clock::now();
                const bool hotkey = realtimeWindow && realtimeWindow->takeDebugCaptureRequest();
                if (hotkey) static_cast<void>(liveDebug->request("F8 player marker"));
                if (hotkey || now >= nextDebugPoll) {
                    if (const auto reason = liveDebug->pollRequest()) {
                        debugAttachNextFrame = true;
                        std::cout << "debug-capture requested: " << *reason << '\n';
                    }
                    if (liveDebug->hasError() && liveDebug->lastError() != debugReportedError) {
                        debugReportedError = liveDebug->lastError();
                        std::cerr << "warning: live debugger: " << debugReportedError << '\n';
                    }
                    nextDebugPoll = now + std::chrono::milliseconds(250);
                }
                auto snapshot = mmio.video().captureDebugSnapshot(liveDebug->isCapturing());
                auto metadata = debugMetadata();
                metadata.step = snapshot.contextStep;
                metadata.block = snapshot.contextBlock;
                metadata.caller = snapshot.contextCaller;
                if (debugAttachNextFrame) {
                    for (const auto base : {0x00200000U, 0x06000000U}) {
                        std::vector<std::uint8_t> bytes(0x100000U);
                        for (std::size_t offset = 0; offset < bytes.size(); ++offset)
                            bytes[offset] = state.readRaw8(static_cast<saturn::core::Address>(base + offset));
                        metadata.attachments.push_back({base == 0x00200000U ? "ram_low.bin" : "ram_high.bin",
                                                        std::move(bytes)});
                    }
                    std::vector<std::uint8_t> soundRam(saturn::platform::ScspRuntime::kRamSize);
                    for (std::size_t offset = 0; offset < soundRam.size(); ++offset)
                        soundRam[offset] = static_cast<std::uint8_t>(mmio.scsp().read(
                            static_cast<saturn::core::Address>(saturn::platform::ScspRuntime::kRamBase + offset), 1U).value_or(0U));
                    metadata.attachments.push_back({"scsp_ram.bin", std::move(soundRam)});
                    std::ostringstream cpuSlots;
                    cpuSlots << "slot,value_hex\n";
                    for (const auto& [slot, value] : state.stateSlots())
                        cpuSlots << std::dec << slot << ",0x" << std::hex << value << '\n';
                    const auto slotText = cpuSlots.str();
                    metadata.attachments.push_back({"cpu_slots.csv", {slotText.begin(), slotText.end()}});
                    debugAttachNextFrame = false;
                }
                liveDebug->recordFrame(std::move(snapshot), std::move(metadata));
            }
            if (arguments.frameDirectory) {
                ++presentedFrames;
                if (arguments.frameCaptureAfterBlocks) {
                    if (!frameCaptureArmed || frameCaptured) return;
                    const auto path = directory / "captured-frame.ppm";
                    if (!writeFramePpm(path, frame)) {
                        frameWriteFailure = true;
                        return;
                    }
                    frameCaptured = true;
                    frameCaptureArmed = false;
                    std::cout << "frame-capture sequence=" << frame.sequence
                              << " armed-at-block=" << frameCaptureArmedAt
                              << " path=" << path.string() << std::endl;
                    return;
                }
                if (arguments.frameChangesOnly && previous.width == frame.width &&
                    previous.height == frame.height && previous.pixels == frame.pixels) return;
                std::ostringstream name;
                name << "frame_" << std::setw(6) << std::setfill('0') << frame.sequence << ".ppm";
                if (!writeFramePpm(directory / name.str(), frame)) {
                    frameWriteFailure = true;
                    return;
                }
                if (arguments.frameChangesOnly) previous = frame;
            }
        });
        if (arguments.frameDirectory && !arguments.frameCaptureAfterBlocks) {
            const auto diagnosticsDirectory = directory / "video-diagnostics";
            mmio.video().setDiagnosticSink([diagnosticsDirectory, &frameWriteFailure](
                                               const saturn::platform::VideoDiagnosticSnapshot& snapshot) {
                if (!writeVideoDiagnostics(diagnosticsDirectory, snapshot)) {
                    frameWriteFailure = true;
                }
            });
        } else if (arguments.frameDirectory && arguments.frameCaptureAfterBlocks) {
            const auto diagnosticsDirectory = directory / "video-diagnostics";
            mmio.video().setDiagnosticSink([diagnosticsDirectory, &frameWriteFailure, &mmio, &state,
                                            &frameCaptureArmed, &frameCaptured](
                                               const saturn::platform::VideoDiagnosticSnapshot& snapshot) {
                if (!frameCaptureArmed || frameCaptured) return;
                if (!writeVideoDiagnostics(diagnosticsDirectory, snapshot)) {
                    frameWriteFailure = true;
                }
                std::ostringstream vdp1Prefix;
                vdp1Prefix << "frame_" << std::setw(6) << std::setfill('0')
                           << snapshot.composed.sequence << '_';
                if (!writeVdp1CommandTrace(diagnosticsDirectory, mmio.video(),
                                           vdp1Prefix.str())) {
                    frameWriteFailure = true;
                }
                // Match the frame to the guest's task, stack and sprite data.
                // Raw RAM reads avoid invoking device hooks during capture.
                for (const auto base : {0x00200000U, 0x06000000U}) {
                    std::vector<std::uint8_t> bytes(0x100000U);
                    for (std::size_t offset = 0; offset < bytes.size(); ++offset)
                        bytes[offset] = state.readRaw8(static_cast<saturn::core::Address>(base + offset));
                    std::ofstream file(diagnosticsDirectory /
                        (base == 0x00200000U ? "ram_low.bin" : "ram_high.bin"), std::ios::binary);
                    file.write(reinterpret_cast<const char*>(bytes.data()),
                               static_cast<std::streamsize>(bytes.size()));
                    if (!file) frameWriteFailure = true;
                }
                std::ofstream registers(diagnosticsDirectory / "cpu_slots.csv");
                registers << "slot,value_hex\n";
                for (const auto& [slot, value] : state.stateSlots())
                    registers << std::dec << slot << ",0x" << std::hex << value << '\n';
                if (!registers) frameWriteFailure = true;
                std::vector<std::uint8_t> soundRam(saturn::platform::ScspRuntime::kRamSize);
                for (std::size_t offset = 0; offset < soundRam.size(); ++offset)
                    soundRam[offset] = static_cast<std::uint8_t>(mmio.scsp().read(
                        static_cast<saturn::core::Address>(saturn::platform::ScspRuntime::kRamBase + offset),
                        1U).value_or(0U));
                std::ofstream soundFile(diagnosticsDirectory / "scsp_ram.bin", std::ios::binary);
                soundFile.write(reinterpret_cast<const char*>(soundRam.data()),
                                static_cast<std::streamsize>(soundRam.size()));
                if (!soundFile) frameWriteFailure = true;
                mmio.video().setDiagnosticCaptureEnabled(false);
            });
        }
    }
    if (arguments.blackFrameProbe) {
        mmio.video().captureFirstReenabledBlackFrame(
            [directory = *arguments.blackFrameProbe, &frameWriteFailure](const auto& snapshot) {
                if (!writeVideoDiagnostics(directory, snapshot)) frameWriteFailure = true;
                std::cout << "black-frame-probe sequence=" << snapshot.composed.sequence
                          << " success=" << !frameWriteFailure << std::endl;
            });
    }
    if (arguments.audioOutput || realtimeAudio != nullptr) {
        if (realtimeAudio != nullptr) mmio.scsp().setRealtimeAudioClock(true);
        mmio.scsp().setAudioSink([realtimeAudio, arguments, &capturedAudio](
                                     const saturn::platform::ScspRuntime::AudioFrame& frame) {
            if (realtimeAudio != nullptr) realtimeAudio->onAudioFrame(frame);
            if (arguments.audioOutput) {
            capturedAudio.insert(capturedAudio.end(), frame.samples.begin(), frame.samples.end());
            }
        });
    }
    saturn::platform::BootRomServices bootServices;
    bootServices.attachMmio(mmio);
    bootServices.setExecuteDmpReturns(arguments.continueAfterExecuteDmp);
    bootServices.install(module, state);
    std::optional<LaunchMemoryProbe> memoryProbe;
    if (arguments.trace || arguments.traceGame) {
        memoryProbe.emplace(bootServices, mmio, state);
    }
    std::optional<LaunchMemoryProbe> cdFifoProbe;
    if (arguments.traceCdSectors && !memoryProbe) {
        cdFifoProbe.emplace(bootServices, mmio, state, true);
    }
    std::optional<SpriteSourceWriteProbe> spriteSourceProbe;
    if (arguments.frameCaptureAfterBlocks) {
        auto& downstream = memoryProbe
            ? static_cast<saturn::sir::MemoryInterceptor&>(*memoryProbe)
            : cdFifoProbe
                ? static_cast<saturn::sir::MemoryInterceptor&>(*cdFifoProbe)
                : static_cast<saturn::sir::MemoryInterceptor&>(bootServices);
        spriteSourceProbe.emplace(downstream, state);
        state.setMemoryInterceptor(&*spriteSourceProbe);
    } else if (memoryProbe) {
        state.setMemoryInterceptor(&*memoryProbe);
    } else if (cdFifoProbe) {
        state.setMemoryInterceptor(&*cdFifoProbe);
    } else {
        state.setMemoryInterceptor(&bootServices);
    }
    std::optional<saturn::platform::CdRuntime::DmaTransfer> resourceDma;
    if (arguments.resourcePath && arguments.resourceBase) {
        resourceDma = mmio.cd().dmaReadFile(arguments.resourcePath->string(),
                                             *arguments.resourceBase, state);
        if (!resourceDma || !resourceDma->complete) {
            std::cerr << "error: unable to DMA CD resource "
                      << arguments.resourcePath->string() << "\n";
            return 1;
        }
    }
    saturn::platform::ScuEventScheduler scheduler(arguments.vblankPeriodBlocks);
    saturn::platform::SlaveSh2 slave;
    // NTSC, 320-dot CPU clock. Tie FRT to the same guest time as video/PCM;
    // eight cycles/block becomes five times too slow at the interactive budget.
    mmio.setCpuTiming(26'874'100U, scheduler.vblankPeriod() * 60U);
    saturn::sh2::RuntimeBlockResolver resolver(image, module, &state);
    saturn::sir::Win64JitBlockExecutor nativeJit;
    std::size_t staticBlockCount{};
    const auto* staticBlocks = builtInStaticBlocks(staticBlockCount);
    saturn::sir::StaticNativeBlockExecutor staticNative(
        module, std::span<const saturn::sir::StaticNativeBlockEntry>{
                    staticBlocks, staticBlockCount});
    std::size_t interpreterFallbackBlocks{};
    bool reachedExpectedEntry = false;
    constexpr std::size_t kRecentBlockLimit = 64U;
    std::array<std::pair<saturn::core::Address, std::size_t>, kRecentBlockLimit>
        recentBlocks{};
    std::size_t recentBlockCount = 0U;
    constexpr saturn::core::Address kScuDispatcher = 0x060303CCU;
    std::array<std::pair<saturn::core::Address, std::size_t>, kRecentBlockLimit>
        recentForeground{};
    std::size_t recentForegroundCount = 0U;
    std::size_t acceleratedByteFills{};
    std::size_t acceleratedWordFills{};
    std::size_t acceleratedWordCopies{};
    saturn::sir::ProgramExecutionOptions options;
    options.maxSteps = arguments.maxSteps;
    options.refreshKnownBlocksOnWrite = true;
    if (!arguments.forceInterpreter && (staticBlockCount > 0U ||
                              saturn::sir::Win64JitBlockExecutor::available())) {
        options.executeBlock = [&staticNative, &nativeJit, &interpreterFallbackBlocks](const saturn::sir::BasicBlock& block,
                                            saturn::sir::MachineState& machineState) {
            if (auto result = staticNative.execute(block, machineState)) return result;
            auto result = nativeJit.execute(block, machineState);
            if (!result) ++interpreterFallbackBlocks;
            return result;
        };
        if (staticBlockCount == 0U) {
            options.executeChain = [&nativeJit](const saturn::sir::BasicBlock& block,
                                                saturn::sir::MachineState& machineState,
                                                std::size_t maxBlocks,
                                                const saturn::sir::BlockChainContinuation& continuation) {
                return nativeJit.executeChain(block, machineState, maxBlocks, continuation);
            };
        }
    }
    options.resolveBlock = [&resolver, &bootServices](saturn::core::Address address) {
        if (auto* service = bootServices.resolve(address)) {
            return service;
        }
        return resolver.resolve(address);
    };
    options.resolveBlockAtStep = [&resolver, &bootServices](saturn::core::Address address,
                                                            std::size_t step) {
        if (auto* service = bootServices.resolve(address)) {
            return service;
        }
        return resolver.resolve(address, step);
    };
    auto pokeIndex = std::size_t{};
    auto watchNextStep = std::size_t{};
    std::optional<std::filesystem::path> watchDirectory;
    if (arguments.watchAddress) {
        watchDirectory = arguments.watchDirectory
            ? *arguments.watchDirectory
            : std::filesystem::path("build/tmp/watch");
        std::filesystem::create_directories(*watchDirectory);
        watchNextStep = arguments.watchFrom;
    }
    options.beforeBlock = [&bootServices, &acceleratedByteFills, &acceleratedWordFills,
                           &acceleratedWordCopies, &realtime, &memoryProbe, expectedEntry,
                           &mmio, &arguments, &watchDirectory, &padSequenceEvents, &x4PlayerSamplesFile,
                           &pokeIndex, &watchNextStep,
                           &x4PlayerSampleCount, &x4PlayerSampleWriteFailure,
                           captureX4PlayerSamples = arguments.x4PlayerSamplesOutput.has_value(),
                           padSequenceNextEvent = std::size_t{},
                           &delayedPads, &configuredPads, &configuredPadStates,
                           padAfterBlocks = arguments.padAfterBlocks,
                           padReleaseAfterBlocks = arguments.padReleaseAfterBlocks,
                           padSecondPressAfterBlocks = arguments.padSecondPressAfterBlocks,
                           padSecondReleaseAfterBlocks = arguments.padSecondReleaseAfterBlocks,
                           padThirdPressAfterBlocks = arguments.padThirdPressAfterBlocks,
                           padThirdReleaseAfterBlocks = arguments.padThirdReleaseAfterBlocks,
                           padLiveAfterBlocks = arguments.padLiveAfterBlocks,
                           padLiveHandedOff = false,
                           frameCaptureAfterBlocks = arguments.frameCaptureAfterBlocks,
                           &frameCaptureArmed, &frameCaptureArmedAt, &frameCaptured,
                           continueAfterExecuteDmp = arguments.continueAfterExecuteDmp](
                              const saturn::sir::BasicBlock& block,
                              saturn::sir::MachineState& state, std::size_t step) mutable {
        // Scripted debug writes, used by the long automated routes to keep the
        // player alive. They run once per step and never change normal runs.
        while (pokeIndex < arguments.pokes.size() &&
               step >= std::get<0>(arguments.pokes[pokeIndex])) {
            const auto& [at, address, value, width] = arguments.pokes[pokeIndex];
            static_cast<void>(at);
            if (width == 1) {
                state.write8(address, static_cast<std::uint8_t>(value));
            } else if (width == 2) {
                state.write16BE(address, static_cast<std::uint16_t>(value));
            } else {
                state.write32BE(address, value);
            }
            ++pokeIndex;
        }
        if (watchDirectory && arguments.watchAddress && step >= watchNextStep) {
            const auto path = *watchDirectory /
                ("watch-" + std::to_string(step) + ".bin");
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (output) {
                for (std::size_t offset = 0; offset < arguments.watchLength; ++offset) {
                    output.put(static_cast<char>(state.readRaw8(
                        static_cast<saturn::core::Address>(*arguments.watchAddress + offset))));
                }
            }
            watchNextStep = step + arguments.watchPeriod;
        }
        if (step >= padAfterBlocks) {
            for (std::size_t port = 0; port < delayedPads.size(); ++port) {
                if (!delayedPads[port]) continue;
                mmio.smpc().setPad(port, *delayedPads[port]);
                mmio.recordPadInput(step, port, *delayedPads[port]);
                delayedPads[port].reset();
            }
        }
        while (padSequenceNextEvent < padSequenceEvents.size() &&
               step >= padSequenceEvents[padSequenceNextEvent].first) {
            if (captureX4PlayerSamples) {
                constexpr saturn::core::Address kX4Player = 0x060545A0U;
                const auto readRaw16 = [&state](saturn::core::Address address) {
                    return static_cast<std::uint16_t>(
                        (static_cast<std::uint16_t>(state.readRaw8(address)) << 8U) |
                        state.readRaw8(address + 1U));
                };
                const auto readRaw32 = [&state](saturn::core::Address address) {
                    return (static_cast<std::uint32_t>(state.readRaw8(address)) << 24U) |
                           (static_cast<std::uint32_t>(state.readRaw8(address + 1U)) << 16U) |
                           (static_cast<std::uint32_t>(state.readRaw8(address + 2U)) << 8U) |
                           state.readRaw8(address + 3U);
                };
                const auto& event = padSequenceEvents[padSequenceNextEvent];
                x4PlayerSamplesFile << padSequenceNextEvent << ',' << event.first << ','
                    << step << ',' << saturn::sh2::formatAddress(block.address()) << ','
                    << mmio.video().lastFrame().sequence << ",0x" << std::hex
                    << std::uppercase << std::setw(4) << std::setfill('0')
                    << mmio.smpc().pad(0U).pressed << ",0x" << std::setw(4)
                    << event.second.pressed << ','
                    << saturn::sh2::formatAddress(readRaw32(kX4Player + 0x08U)) << ','
                    << saturn::sh2::formatAddress(readRaw32(kX4Player + 0x20U))
                    << ",0x" << std::setw(4) << readRaw16(kX4Player + 0x7CU)
                    << ",0x" << std::setw(4) << readRaw16(kX4Player + 0x7EU)
                    << ",0x" << std::setw(4) << readRaw16(kX4Player + 0x80U)
                    << ",0x" << std::setw(2) << static_cast<unsigned>(
                           state.readRaw8(kX4Player + 0x84U))
                    << ",0x" << std::setw(2) << static_cast<unsigned>(
                           state.readRaw8(kX4Player + 0x89U))
                    << ",0x" << std::setw(2) << static_cast<unsigned>(
                           state.readRaw8(kX4Player + 0xC3U))
                    << std::dec << std::nouppercase << std::setfill(' ') << '\n';
                ++x4PlayerSampleCount;
                if (!x4PlayerSamplesFile) x4PlayerSampleWriteFailure = true;
            }
            mmio.smpc().setPad(0U, padSequenceEvents[padSequenceNextEvent].second);
            mmio.recordPadInput(step, 0U,
                                padSequenceEvents[padSequenceNextEvent].second);
            ++padSequenceNextEvent;
        }
        if (padLiveAfterBlocks && !padLiveHandedOff && step >= *padLiveAfterBlocks) {
            mmio.smpc().setPad(0U, {});
            mmio.recordPadInput(step, 0U, {});
            if (realtime) realtime->setInputScripted(0U, false);
            padLiveHandedOff = true;
            std::cout << "  live input: port 1 at block " << step << std::endl;
        }
        if (padReleaseAfterBlocks && step >= *padReleaseAfterBlocks) {
            for (std::size_t port = 0; port < configuredPads.size(); ++port) {
                if (configuredPads[port]) {
                    mmio.smpc().setPad(port, {});
                    mmio.recordPadInput(step, port, {});
                }
            }
            padReleaseAfterBlocks.reset();
        }
        if (padSecondPressAfterBlocks && step >= *padSecondPressAfterBlocks) {
            for (std::size_t port = 0; port < configuredPadStates.size(); ++port) {
                if (configuredPadStates[port]) {
                    mmio.smpc().setPad(port, *configuredPadStates[port]);
                    mmio.recordPadInput(step, port, *configuredPadStates[port]);
                }
            }
            padSecondPressAfterBlocks.reset();
        }
        if (padSecondReleaseAfterBlocks && step >= *padSecondReleaseAfterBlocks) {
            for (std::size_t port = 0; port < configuredPads.size(); ++port) {
                if (configuredPads[port]) {
                    mmio.smpc().setPad(port, {});
                    mmio.recordPadInput(step, port, {});
                }
            }
            padSecondReleaseAfterBlocks.reset();
        }
        if (padThirdPressAfterBlocks && step >= *padThirdPressAfterBlocks) {
            for (std::size_t port = 0; port < configuredPadStates.size(); ++port) {
                if (configuredPadStates[port]) {
                    mmio.smpc().setPad(port, *configuredPadStates[port]);
                    mmio.recordPadInput(step, port, *configuredPadStates[port]);
                }
            }
            padThirdPressAfterBlocks.reset();
        }
        if (padThirdReleaseAfterBlocks && step >= *padThirdReleaseAfterBlocks) {
            for (std::size_t port = 0; port < configuredPads.size(); ++port) {
                if (configuredPads[port]) {
                    mmio.smpc().setPad(port, {});
                    mmio.recordPadInput(step, port, {});
                }
            }
            padThirdReleaseAfterBlocks.reset();
        }
        if (frameCaptureAfterBlocks && !frameCaptured && !frameCaptureArmed &&
            step >= *frameCaptureAfterBlocks) {
            frameCaptureArmed = true;
            frameCaptureArmedAt = step;
            mmio.video().setDiagnosticCaptureEnabled(true);
        }
        // Video frames also pump messages. Poll between them at a bounded
        // cadence instead of making a Win32 syscall for every SH-2 block.
        if (realtime && (step & 0x3FFU) == 0U) {
            realtime->pump();
            if (realtime->shouldQuit()) throw FrontendQuit{};
        }
        if (memoryProbe) memoryProbe->observeBlockTransition();
        bootServices.executeService(block, state);
        // SYS_EXECDMP is the BIOS handoff: in a real console it never returns
        // to the AIP caller.  The old diagnostic continuation intentionally
        // returned through PR, which parked launch mode in the AIP's CD
        // interrupt dispatcher and produced a black, inert window. Redirect
        // the synthetic RTS to the loaded program entry only when the caller
        // explicitly requested continuation (plain `boot` keeps its sleep
        // boundary for deterministic harness tests).
        if (continueAfterExecuteDmp &&
            block.address() == saturn::platform::BootRomServices::kExitStubAddress) {
            state.writeState(saturn::sir::kProcedureRegisterStateSlot, expectedEntry);
        }
        // 0.BIN contains two hot byte-fill helpers at these addresses. Their
        // blocks write R4 to [R6], increment R6, and branch while R6<R7.
        // Executing every byte as a SIR block makes the real boot appear to
        // stall in this foreground loop. Collapse only this verified pattern;
        // all writes still pass through MachineState's memory interceptor.
        if (block.address() == 0x06010606U || block.address() == 0x06010680U) {
            const auto begin = state.readState(saturn::sh2::stateSlot(
                saturn::sh2::Register::R6));
            // R7 is a pointer to the end-address cell (the block at 0x10684
            // executes MOV.L @R7,R2 before comparing R6). Reading R7 itself
            // makes the guard reject the real copy range and leaves the
            // emulator spinning one byte at a time.
            const auto endPointer = state.readState(saturn::sh2::stateSlot(
                saturn::sh2::Register::R7));
            const auto end = state.read32BE(endPointer);
            if (end <= begin || static_cast<std::uint64_t>(end) - begin > 0x02000000ULL) {
                return;
            }
            const auto value = static_cast<std::uint8_t>(state.readState(
                saturn::sh2::stateSlot(saturn::sh2::Register::R4)));
            for (auto address = begin; address < end; ++address) state.write8(address, value);
            // Let the real block execute its final byte/increment and then
            // take the fall-through path instead of re-entering the loop.
            state.writeState(saturn::sh2::stateSlot(saturn::sh2::Register::R6), end - 1U);
            ++acceleratedByteFills;
            return;
        }

        // The AIP uses the same style of helper for word-sized clears.  At
        // 0x06002F92 R4 is the destination, R6 is the inclusive low-24-bit
        // end marker, and R7 is the masked loop counter.  Replay all but the
        // final store through the normal bus, then seed R7 so the real block
        // executes that final store and exits through its delay slot.
        if (block.address() == 0x06002F92U) {
            const auto begin = state.readState(saturn::sh2::stateSlot(
                saturn::sh2::Register::R4));
            const auto endMarker = state.readState(saturn::sh2::stateSlot(
                saturn::sh2::Register::R6));
            const auto firstCounter = begin & endMarker;
            if (endMarker < firstCounter || ((endMarker - firstCounter) & 1U) != 0U) {
                return;
            }
            const auto words = static_cast<std::uint64_t>(
                (endMarker - firstCounter) / 2U) + 1U;
            if (words <= 1U || words > 0x01000000ULL) return;
            const auto last = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(begin) + (words - 1U) * 2U);
            const auto value = state.readState(saturn::sh2::stateSlot(
                saturn::sh2::Register::R5));
            for (auto address = begin; address < last; address += 2U) {
                state.write16BE(address, static_cast<std::uint16_t>(value));
            }
            state.writeState(saturn::sh2::stateSlot(saturn::sh2::Register::R4), last);
            state.writeState(saturn::sh2::stateSlot(saturn::sh2::Register::R7), endMarker);
            ++acceleratedWordFills;
            return;
        }

        // A second AIP helper copies a short table into VDP1/VDP2 with
        // MOV.W @R5+,R3 / MOV.W R3,@R4.  It uses R6 as a byte count and
        // exits when the post-increment counter reaches that count.
        if (block.address() != 0x06002F7AU) return;
        const auto count = state.readState(saturn::sh2::stateSlot(
            saturn::sh2::Register::R6));
        if (count < 2U || (count & 1U) != 0U || count > 0x01000000U) return;
        const auto words = count / 2U;
        const auto source = state.readState(saturn::sh2::stateSlot(
            saturn::sh2::Register::R5));
        const auto destination = state.readState(saturn::sh2::stateSlot(
            saturn::sh2::Register::R4));
        if (words <= 1U) return;
        for (std::uint32_t index = 0U; index + 1U < words; ++index) {
            state.write16BE(destination + index * 2U,
                             state.read16BE(source + index * 2U));
        }
        state.writeState(saturn::sh2::stateSlot(saturn::sh2::Register::R4),
                         destination + (words - 1U) * 2U);
        state.writeState(saturn::sh2::stateSlot(saturn::sh2::Register::R5),
                         source + (words - 1U) * 2U);
        state.writeState(saturn::sh2::stateSlot(saturn::sh2::Register::R7), count - 2U);
        ++acceleratedWordCopies;
    };
    options.interrupt = [&scheduler, &mmio, &slave, &bootServices, &options](const saturn::sir::BasicBlock& block,
                                            saturn::sir::MachineState& state,
                                            std::size_t step) {
        if (!slave.tick(state, bootServices, mmio, step,
                        options.resolveBlockAtStep, options.executeBlock)) {
            throw SlaveExecutionFailure{slave.pc(), step};
        }
        return scheduler.beforeBlock(block, state, step, mmio);
    };
    options.observeBlock = [&reachedExpectedEntry, &recentBlocks, &recentBlockCount,
                            &recentForeground, &recentForegroundCount, expectedEntry,
                            &memoryProbe, &mmio, &slave, traceGame = arguments.traceGame,
                            reportProgress = arguments.controlTraceOutput.has_value(),
                            kScuDispatcher](const saturn::sir::BasicBlock& block,
                                            std::size_t step) {
        if (traceGame && memoryProbe && step != 0U && step % 1'000'000U == 0U) {
            const auto controller = memoryProbe->audioController();
            const auto field = [&](std::uint32_t offset) {
                return controller == 0U ? 0U : memoryProbe->ram32(controller + offset);
            };
            const auto& frame = mmio.video().lastFrame();
            std::uint64_t hash = 14695981039346656037ULL;
            std::size_t nonblack{};
            for (const auto pixel : frame.pixels) {
                hash = (hash ^ pixel) * 1099511628211ULL;
                nonblack += (pixel & 0xFFFFFFU) != 0U ? 1U : 0U;
            }
            std::cout << "[progress] step=" << step
                      << " block=" << saturn::sh2::formatAddress(block.address())
                      << " state=" << field(0x04U)
                      << " stream-written=" << saturn::sh2::formatAddress(field(0x54U))
                      << " stream-read=" << saturn::sh2::formatAddress(field(0x58U))
                      << " descriptor=" << field(0x0CU) << " active=" << field(0x20U)
                      << " consumed-descriptors=" << memoryProbe->gfsProducerRecordCount()
                      << " audio-consumer=" << field(0x90U) << " audio-written=" << field(0x98U)
                      << " audio-frames=" << mmio.scsp().stats().audioFrames
                      << " pcm-updates=" << mmio.scsp().stats().pcmStatusUpdates
                      << " video-frames=" << frame.sequence << " pixels=" << nonblack
                      << " frame-hash=" << hash
                      << " pad=" << saturn::sh2::formatAddress(memoryProbe->ram32(0x0605EA00U))
                      << " smif=" << saturn::sh2::formatAddress(memoryProbe->ram32(0x0606D570U))
                      << " smpc-sr=" << mmio.smpc().read(saturn::platform::SmpcRuntime::kStatus, 1).value_or(0U)
                      << " smpc-pad=" << mmio.smpc().pad(0).pressed
                      << std::endl;
        }
        if (!traceGame && reportProgress && step != 0U && step % 1'000'000U == 0U) {
            std::cout << "[progress] step=" << step
                      << " block=" << saturn::sh2::formatAddress(block.address())
                      << " slave-blocks=" << slave.executedBlocks()
                      << " slave-pc=" << saturn::sh2::formatAddress(slave.pc())
                      << " pad=" << mmio.smpc().pad(0).pressed << std::endl;
        }
        reachedExpectedEntry = reachedExpectedEntry || block.address() == expectedEntry;
        recentBlocks[recentBlockCount % recentBlocks.size()] = {block.address(), step};
        ++recentBlockCount;
        // Keep a second ring that omits the known SCU dispatch trampoline. It
        // makes a foreground poll visible even when interrupts dominate the
        // global trace (the common post-boot failure mode).
        if (block.address() != kScuDispatcher) {
            recentForeground[recentForegroundCount % recentForeground.size()] =
                {block.address(), step};
            ++recentForegroundCount;
        }
    };
    saturn::sir::ProgramExecutionResult execution;
    try {
        execution = saturn::sir::ProgramExecutor{}.run(module.blockAt(entry), state, options);
    } catch (const FrontendQuit&) {
        execution.status = saturn::sir::ProgramStatus::FrontendQuit;
        execution.address = state.readState(saturn::sir::kProgramCounterStateSlot);
    } catch (const saturn::platform::ScspRuntime::DriverError& error) {
        std::cerr << "error: sound CPU: " << error.what() << '\n';
        execution.status = saturn::sir::ProgramStatus::InterpreterFailure;
        execution.address = state.readState(saturn::sir::kProgramCounterStateSlot);
    } catch (const SlaveExecutionFailure& failure) {
        std::cerr << "error: slave SH-2 execution failed at "
                  << saturn::sh2::formatAddress(failure.address) << '\n';
        execution.status = saturn::sir::ProgramStatus::InterpreterFailure;
        execution.address = failure.address;
        execution.steps = failure.step;
    }
    if (liveDebug) {
        liveDebug->flush();
        std::cout << "  debug session: " << liveDebug->directory().string()
                  << ", dropped snapshots: " << liveDebug->droppedSnapshots() << '\n';
        if (liveDebug->hasError())
            std::cerr << "warning: live debugger: " << liveDebug->lastError() << '\n';
    }
    bool controlTraceWriteFailure{};
    if (arguments.controlTraceOutput &&
        !writeControlTrace(*arguments.controlTraceOutput, mmio)) {
        std::cerr << "error: unable to write control trace "
                  << arguments.controlTraceOutput->string() << '\n';
        controlTraceWriteFailure = true;
    }
    if (arguments.x4PlayerSamplesOutput) {
        x4PlayerSamplesFile.flush();
        if (!x4PlayerSamplesFile || x4PlayerSampleWriteFailure) {
            std::cerr << "error: unable to finish X4 player samples "
                      << arguments.x4PlayerSamplesOutput->string() << '\n';
            controlTraceWriteFailure = true;
        } else {
            std::cout << "  X4 player samples recorded: "
                      << x4PlayerSampleCount << '\n';
        }
    }
    if (arguments.aotEntryOutput) {
        std::ofstream entries(*arguments.aotEntryOutput, std::ios::trunc);
        if (!entries) {
            std::cerr << "error: unable to write AOT entry list "
                      << arguments.aotEntryOutput->string() << '\n';
            return 1;
        }
        const auto addresses = resolver.resolvedAddresses();
        for (const auto address : addresses) {
            entries << "0x" << std::uppercase << std::hex << std::setw(8)
                    << std::setfill('0') << address << '\n';
        }
        if (!entries) {
            std::cerr << "error: failed while writing AOT entry list "
                      << arguments.aotEntryOutput->string() << '\n';
            return 1;
        }
        std::cout << "  AOT entry targets recorded: " << addresses.size() << '\n';
    }
    if (arguments.aotModuleOutput) {
        std::ofstream output(*arguments.aotModuleOutput,
                             std::ios::binary | std::ios::trunc);
        if (!output) {
            std::cerr << "error: unable to write AOT module "
                      << arguments.aotModuleOutput->string() << '\n';
            return 1;
        }
        const auto report = saturn::sir::emitStaticModuleCpp(module, output);
        output.close();
        if (!report.success()) {
            std::cerr << "error: unable to emit AOT module: " << report.error << '\n';
            return 1;
        }
        std::cout << "  AOT module blocks emitted: " << report.blocksEmitted << '\n'
                  << "  AOT module SIR instructions: " << report.instructionsEmitted << '\n'
                  << "  AOT module blocks skipped: " << report.skippedBlocks.size() << '\n';
    }
    if (realtime) realtime->printStats();
    if (vdp1Renderer) {
        std::cout << "  Vulkan VDP1 dispatches: " << vdp1Renderer->dispatchCount()
                  << ", GPU primitives: " << vdp1Renderer->primitiveCount()
                  << ", fence wait ms: " << vdp1Renderer->waitMilliseconds()
                  << ", host copy ms: " << vdp1Renderer->copyMilliseconds() << '\n';
    }
    if (arguments.frameDirectory &&
        !writeVdp2WindowTrace(*arguments.frameDirectory, mmio.accesses())) {
        frameWriteFailure = true;
    }
    if (arguments.vdp2MapTrace && memoryProbe &&
        !writeVdp2MapProducerTrace(*arguments.vdp2MapTrace,
                                   memoryProbe->vdp2MapWrites(),
                                   mmio.scuDma().transferHistory())) {
        frameWriteFailure = true;
    }
    if (arguments.vdp2MapReadTrace &&
        !writeVdp2MapReadTrace(*arguments.vdp2MapReadTrace,
                               mmio.video().mapReadTrace(),
                               mmio.video().mapStateTrace())) {
        frameWriteFailure = true;
    }
    if (arguments.frameDirectory &&
        !writeScuDmaTrace(*arguments.frameDirectory, mmio.accesses())) {
        frameWriteFailure = true;
    }
    if (arguments.frameDirectory && memoryProbe &&
        !writeDmaDescriptorTrace(*arguments.frameDirectory,
                                 memoryProbe->dmaDescriptorRecords())) {
        frameWriteFailure = true;
    }
    if (arguments.frameDirectory &&
        !writeScuDmaTransferTrace(*arguments.frameDirectory,
                                  mmio.scuDma().transferHistory(),
                                  mmio.scuDma().vdp1TextureTransferHistory(),
                                  mmio.scuDma().spriteSourceBufferTransferHistory())) {
        frameWriteFailure = true;
    }
    if (arguments.frameDirectory && spriteSourceProbe &&
        !writeSpriteSourceWriteTrace(*arguments.frameDirectory, *spriteSourceProbe)) {
        frameWriteFailure = true;
    }
    if (arguments.traceCdSectors && arguments.frameDirectory &&
        !writeCdSectorTrace(*arguments.frameDirectory, mmio.cd())) {
        frameWriteFailure = true;
    }
    if (arguments.traceCdSectors && arguments.frameDirectory) {
        const auto* fifoReads = memoryProbe
            ? &memoryProbe->cdFifoReadRecords()
            : cdFifoProbe ? &cdFifoProbe->cdFifoReadRecords() : nullptr;
        if (fifoReads != nullptr &&
            !writeCdFifoReadTrace(*arguments.frameDirectory, *fifoReads)) {
            frameWriteFailure = true;
        }
    }
    if (arguments.frameDirectory && memoryProbe &&
        !writeGfsSourceTrace(*arguments.frameDirectory, *memoryProbe)) {
        frameWriteFailure = true;
    }
    if (arguments.frameDirectory && memoryProbe &&
        !writeGfsTaskConfigTrace(*arguments.frameDirectory, *memoryProbe)) {
        frameWriteFailure = true;
    }
    if (arguments.frameDirectory && memoryProbe &&
        !writeGfsParserStackTrace(*arguments.frameDirectory, *memoryProbe)) {
        frameWriteFailure = true;
    }
    if (arguments.frameDirectory && memoryProbe &&
        !writeGfsParserDataTrace(*arguments.frameDirectory, *memoryProbe)) {
        frameWriteFailure = true;
    }
    if (arguments.frameDirectory && memoryProbe &&
        !writeGfsParserSourceTrace(*arguments.frameDirectory, *memoryProbe)) {
        frameWriteFailure = true;
    }
    if (arguments.frameDirectory && memoryProbe &&
        !writeGfsDecodeTrace(*arguments.frameDirectory, *memoryProbe)) {
        frameWriteFailure = true;
    }
    if (arguments.frameDirectory && memoryProbe &&
        !writeGfsStabCallbackTrace(*arguments.frameDirectory, *memoryProbe)) {
        frameWriteFailure = true;
    }
    if (arguments.frameDirectory && memoryProbe &&
        !writeGfsOutputCoverageTrace(*arguments.frameDirectory, *memoryProbe)) {
        frameWriteFailure = true;
    }
    if (arguments.frameDirectory && memoryProbe &&
        !writeGfsSkipTrace(*arguments.frameDirectory, *memoryProbe)) {
        frameWriteFailure = true;
    }
    if (arguments.frameDirectory &&
        !writeVdp2LayerTrace(*arguments.frameDirectory, mmio.accesses())) {
        frameWriteFailure = true;
    }
    if (arguments.frameDirectory &&
        !writeVdp1CommandTrace(*arguments.frameDirectory, mmio.video())) {
        frameWriteFailure = true;
    }
    if (arguments.frameDirectory &&
        !writeVdp1RegisterTrace(*arguments.frameDirectory, mmio.accesses())) {
        frameWriteFailure = true;
    }
    std::cout << "Saturn boot execution\n"
              << "  AIP entry: " << saturn::sh2::formatAddress(entry) << '\n'
              << "  expected program entry: "
              << saturn::sh2::formatAddress(expectedEntry) << '\n'
              << "  boot image: " << (arguments.ipImage ? "IP.BIN" : "AIP") << '\n'
              << "  segments: " << image.segmentCount() << '\n'
              << "  status: " << programStatusName(execution.status) << '\n'
              << "  completed block steps: " << execution.steps << '\n'
              << "  last block: " << saturn::sh2::formatAddress(execution.address) << '\n'
              << "  native JIT blocks compiled: " << nativeJit.compiledBlockCount() << '\n'
              << "  native JIT blocks executed: " << nativeJit.executedBlockCount() << '\n'
              << "  native JIT chained transfers: " << nativeJit.chainedTransferCount() << '\n'
              << "  interpreter fallback blocks: " << interpreterFallbackBlocks << '\n'
              << "  final regs: r0=" << saturn::sh2::formatAddress(state.readState(saturn::sh2::stateSlot(saturn::sh2::Register::R0)))
              << " r12=" << saturn::sh2::formatAddress(state.readState(saturn::sh2::stateSlot(saturn::sh2::Register::R12)))
              << " r14=" << saturn::sh2::formatAddress(state.readState(saturn::sh2::stateSlot(saturn::sh2::Register::R14)))
              << " sr=" << saturn::sh2::formatAddress(state.readState(saturn::sh2::stateSlot(saturn::sh2::Register::SR))) << '\n'
              << "  handoff: " << (reachedExpectedEntry ? "reached" : "not-reached") << '\n'
              << "  boot service slots: " << bootServices.slots().size() << '\n'
              << "  slave SH-2 blocks executed: " << slave.executedBlocks() << '\n'
              << "  slave SH-2 PC: " << saturn::sh2::formatAddress(slave.pc()) << '\n'
              << "  CD image: " << (mmio.cd().hasImage() ? "loaded" : "absent") << '\n'
              << "  CD sectors: " << mmio.cd().sectorCount() << '\n'
              << "  CD state: hirq=" << saturn::sh2::formatAddress(mmio.cd().hirq())
              << ", cr1=" << saturn::sh2::formatAddress(mmio.cd().commandRegister(0U))
              << ", cr2=" << saturn::sh2::formatAddress(mmio.cd().commandRegister(1U))
              << ", cr3=" << saturn::sh2::formatAddress(mmio.cd().commandRegister(2U))
              << ", cr4=" << saturn::sh2::formatAddress(mmio.cd().commandRegister(3U))
              << '\n'
              << "  CD protocol: commands=" << mmio.cd().stats().commands
              << ", last=0x" << std::hex << static_cast<unsigned>(mmio.cd().stats().lastInstruction)
              << std::dec << ", hirq-r=" << mmio.cd().stats().hirqReads
              << ", hirq-w=" << mmio.cd().stats().hirqWrites
              << ", fifo-r=" << mmio.cd().stats().fifoReads
              << ", fifo-w=" << mmio.cd().stats().fifoWrites
              << ", sectors=" << mmio.cd().stats().sectorsPrepared
              << ", last-partition=" << static_cast<unsigned>(mmio.cd().stats().lastPartition)
              << ", retained=" << mmio.cd().stats().lastPartitionSectorCount
              << ", stream=" << mmio.cd().stats().lastStreamingCursor
              << "/" << mmio.cd().stats().lastStreamingSectorCount
              << ", 0x51=" << mmio.cd().stats().sectorNumberQueries
              << "->(" << saturn::sh2::formatAddress(mmio.cd().stats().lastSectorNumberResponse[0])
              << "," << saturn::sh2::formatAddress(mmio.cd().stats().lastSectorNumberResponse[3])
              << ",partition=" << static_cast<unsigned>(mmio.cd().stats().lastSectorNumberPartition)
              << "), hirq51=" << saturn::sh2::formatAddress(mmio.cd().stats().lastSectorNumberHirq)
              << ", last-hirq=0x" << std::hex << mmio.cd().stats().lastHirq
              << std::dec << "\n  CD HIRQ reads by factor:";
    std::cout << "\n  CD 0x52 calculate: commands=" << mmio.cd().stats().calculateCommands
              << " waits=" << mmio.cd().stats().calculateWaits
              << " request=(" << mmio.cd().stats().lastCalculateRegisters[0]
              << "," << mmio.cd().stats().lastCalculateRegisters[1]
              << "," << mmio.cd().stats().lastCalculateRegisters[2]
              << "," << mmio.cd().stats().lastCalculateRegisters[3]
              << ") result=" << saturn::sh2::formatAddress(
                  mmio.cd().stats().lastCalculateResult) << '\n';
    std::cout << "  CD data requests: transfer-sectors="
              << mmio.cd().stats().transferSectorRequests
              << " transfer-waits=" << mmio.cd().stats().transferWaits
              << " wait-last=(offset=0x" << std::hex
              << mmio.cd().stats().lastTransferWaitOffset
              << ",count=0x" << mmio.cd().stats().lastTransferWaitCount
              << ",retained=0x" << mmio.cd().stats().lastTransferWaitRetained
              << " last=(instruction=0x" << std::hex
              << static_cast<unsigned>(mmio.cd().stats().lastDataInstruction)
              << ",offset=0x" << mmio.cd().stats().lastDataOffset
              << ",count=0x" << mmio.cd().stats().lastDataCount
              << ",retained=0x" << mmio.cd().stats().lastDataRetained
              << ")" << std::dec << '\n';
    for (std::size_t factor = 0U; factor < mmio.cd().stats().hirqReadFactors.size(); ++factor) {
        const auto count = mmio.cd().stats().hirqReadFactors[factor];
        if (count != 0U) std::cout << " b" << factor << "=" << count;
    }
    std::cout << '\n';
    const auto& cdStats = mmio.cd().stats();
    std::cout << "\n  CD instruction totals:";
    for (std::size_t opcode = 0U; opcode < cdStats.instructionCounts.size(); ++opcode) {
        if (cdStats.instructionCounts[opcode] != 0U) {
            std::cout << " 0x" << std::hex << opcode << std::dec
                      << '=' << cdStats.instructionCounts[opcode];
        }
    }
    std::cout << "\n  CD command history:";
    const auto cdHistoryShown = std::min(cdStats.commandHistoryCount,
                                         cdStats.commandHistory.size());
    const auto cdHistoryStart = cdStats.commandHistoryCount > cdHistoryShown
        ? cdStats.commandHistoryCount % cdStats.commandHistory.size() : 0U;
    for (std::size_t index = 0U; index < cdHistoryShown; ++index) {
        const auto historyIndex = (cdHistoryStart + index) % cdStats.commandHistory.size();
        std::cout << " 0x" << std::hex << static_cast<unsigned>(
            cdStats.commandHistory[historyIndex]) << "("
            << cdStats.commandRegistersHistory[historyIndex][0] << ","
            << cdStats.commandRegistersHistory[historyIndex][1] << ","
            << cdStats.commandRegistersHistory[historyIndex][2] << ","
            << cdStats.commandRegistersHistory[historyIndex][3] << ")";
    }
    std::cout << std::dec << '\n'
              << "  CD command counts:"
              << " 0x10=" << cdStats.instructionCounts[0x10U]
              << " 0x11=" << cdStats.instructionCounts[0x11U]
              << " 0x30=" << cdStats.instructionCounts[0x30U]
              << " 0x48=" << cdStats.instructionCounts[0x48U]
              << " 0x51=" << cdStats.instructionCounts[0x51U]
              << " 0x61=" << cdStats.instructionCounts[0x61U]
              << " 0x62=" << cdStats.instructionCounts[0x62U]
              << " 0x63=" << cdStats.instructionCounts[0x63U]
              << " 0x71=" << cdStats.instructionCounts[0x71U]
              << " 0x73=" << cdStats.instructionCounts[0x73U]
              << " 0x74=" << cdStats.instructionCounts[0x74U]
              << " last-play=(" << cdStats.lastPlayRegisters[0]
              << "," << cdStats.lastPlayRegisters[1]
              << "," << cdStats.lastPlayRegisters[2]
              << "," << cdStats.lastPlayRegisters[3] << ")"
              << " filter=(" << static_cast<unsigned>(cdStats.lastStreamingFilter)
              << "->" << static_cast<unsigned>(cdStats.lastStreamingDestination)
              << ",fad=" << cdStats.lastFilterFad
              << ",range=" << cdStats.lastFilterRange
              << ",mode=" << static_cast<unsigned>(cdStats.lastFilterMode)
              << ",true=" << static_cast<unsigned>(cdStats.lastFilterTrueConnection)
              << ",false=" << static_cast<unsigned>(cdStats.lastFilterFalseConnection)
              << ")"
              << " play=(requested=" << cdStats.lastPlayRequested
              << ",available=" << cdStats.lastPlayAvailable
              << ",retained=" << cdStats.lastPlayRetained
              << ",dest=" << static_cast<unsigned>(cdStats.lastPlayDestination)
              << ",after-tick=" << cdStats.lastPlayPartitionCountAfterTick
              << ")"
              << " subcode(ticks=" << cdStats.subcodeTicks
              << ",produced=" << cdStats.subcodeProducedTicks
              << ",blocked-transfer=" << cdStats.subcodeBlockedTransferTicks
              << ",inactive=" << cdStats.subcodeInactiveTicks
              << ",backpressure=" << cdStats.subcodeBackpressureTicks
              << ",invalid-destination=" << cdStats.subcodeInvalidDestinationTicks
              << ",range-complete=" << cdStats.subcodeRangeCompleteTicks
              << ",active=" << cdStats.lastSubcodeActive
              << ",transfer=" << cdStats.lastTransferActive
              << ") last-transfer=(lba=" << cdStats.lastTransferLba
              << ",offset=" << cdStats.lastTransferOffset
              << ",count=" << cdStats.lastTransferCount
              << ",partition=" << static_cast<unsigned>(cdStats.lastTransferPartition)
              << ",instruction=0x" << std::hex
              << static_cast<unsigned>(cdStats.lastTransferInstruction) << std::dec
              << ")"
              << " delete=(commands=" << cdStats.deleteCommands
              << ",waits=" << cdStats.deleteWaits
              << ",transfer-sectors=" << cdStats.transferSectorRequests
              << ",delete-sectors=" << cdStats.deleteSectorRequests
              << ",partition=" << static_cast<unsigned>(cdStats.lastDeletePartition)
              << ",offset=" << cdStats.lastDeleteOffset
              << ",count=" << cdStats.lastDeleteCount
              << ",before=" << cdStats.lastDeleteBefore
              << ",after=" << cdStats.lastDeleteAfter << ")"
              << '\n';
    std::cout << "  CD PLAY history:";
    const auto playShown = std::min(cdStats.playHistoryCount,
                                    cdStats.playHistory.size());
    const auto playStart = cdStats.playHistoryCount > playShown
        ? cdStats.playHistoryCount % cdStats.playHistory.size() : 0U;
    for (std::size_t index = 0U; index < playShown; ++index) {
        const auto& play = cdStats.playHistory[
            (playStart + index) % cdStats.playHistory.size()];
        std::cout << " (" << play[0] << ',' << play[1] << ','
                  << play[2] << ',' << play[3] << ')';
    }
    std::cout << '\n'
              << "  CD HIRQ writes:";
    const auto hirqWritesShown = std::min(cdStats.hirqWriteHistoryCount,
                                          cdStats.hirqWriteHistory.size());
    const auto hirqWritesStart = cdStats.hirqWriteHistoryCount > hirqWritesShown
        ? cdStats.hirqWriteHistoryCount % cdStats.hirqWriteHistory.size() : 0U;
    for (std::size_t index = 0U; index < hirqWritesShown; ++index) {
        const auto historyIndex = (hirqWritesStart + index) % cdStats.hirqWriteHistory.size();
        std::cout << " 0x" << std::hex << cdStats.hirqWriteHistory[historyIndex];
    }
    std::cout << std::dec << '\n'
              << "  CD DMA: "
              << (resourceDma ? arguments.resourcePath->string() + " -> " +
                     saturn::sh2::formatAddress(resourceDma->destination) +
                     " (" + std::to_string(resourceDma->transferred) + " bytes, " +
                     std::to_string(resourceDma->sectors) + " sectors)" : "none") << '\n'
              << "  video frames: " << mmio.video().frameCount() << '\n'
              << "  video compositions: " << mmio.video().compositionCount() << '\n'
              << "  presented frames: " << presentedFrames
              << (frameWriteFailure ? " (write-error)" : "") << '\n'
              << "  dynamic target requests: " << resolver.stats().requests << '\n'
              << "  blocks lowered on demand: " << resolver.stats().loweredBlocks << '\n'
              << "  resolver block re-lowerings: " << resolver.stats().replacedBlocks << '\n'
              << "  resolver re-lowering addresses:";
    for (const auto address : resolver.replacedAddresses()) {
        std::cout << " " << saturn::sh2::formatAddress(address);
    }
    const auto confirmedLiveCodeChanges = std::count_if(
        resolver.replacements().begin(), resolver.replacements().end(),
        [](const auto& replacement) {
            return replacement.previousSignatureKnown &&
                   replacement.previousSignature != replacement.replacementSignature;
        });
    std::cout << "\n  confirmed live-code version changes: "
              << confirmedLiveCodeChanges
              << "\n  resolver re-lowering events:";
    for (const auto& replacement : resolver.replacements()) {
        std::cout << " [step=" << replacement.step
                  << ",address=" << saturn::sh2::formatAddress(replacement.address)
                  << ",old=";
        if (replacement.previousSignatureKnown) {
            std::cout << "0x" << std::hex << replacement.previousSignature << std::dec;
        } else {
            std::cout << "unknown";
        }
        std::cout << ",new=0x" << std::hex << replacement.replacementSignature
                  << std::dec << "]";
    }
    std::cout << '\n'
              << "  static native blocks compiled: " << staticNative.blockCount() << '\n'
              << "  static native blocks executed: " << staticNative.executedBlockCount() << '\n'
              << "  compiled overlay variant rebinds: "
              << staticNative.overlayVariantSelections().size() << '\n'
              << "  compiled overlay variant addresses:";
    for (const auto address : staticNative.overlayVariantSelections()) {
        std::cout << " " << saturn::sh2::formatAddress(address);
    }
    std::cout << '\n'
              << "  foreground blocks observed: " << recentForegroundCount << '\n'
              << "  accelerated byte fills: " << acceleratedByteFills << '\n'
              << "  accelerated word fills: " << acceleratedWordFills << '\n'
              << "  accelerated word copies: " << acceleratedWordCopies << '\n'
              << "  MMIO accesses: " << mmio.accessCount() << " (retained "
              << mmio.accesses().size() << ")"
              << (arguments.traceGame && !arguments.trace
                      ? " (recording disabled by --trace-game)" : "")
              << '\n';
    if (arguments.controlTraceOutput) {
        std::cout << "  control events: " << mmio.controlTrace().size()
                  << " (dropped " << mmio.droppedControlTraceRecords()
                  << ") -> " << arguments.controlTraceOutput->string() << '\n';
    }
    std::array<std::size_t, 10> mmioCounts{};
    for (const auto& access : mmio.accesses()) {
        const auto index = static_cast<std::size_t>(access.space);
        if (index < mmioCounts.size()) {
            ++mmioCounts[index];
        }
    }
    std::cout << "  MMIO by space (retained window): smpc="
              << mmioCounts[static_cast<std::size_t>(saturn::platform::AddressSpace::Smpc)]
              << " vdp1="
              << mmioCounts[static_cast<std::size_t>(saturn::platform::AddressSpace::Vdp1)]
              << " vdp2="
              << mmioCounts[static_cast<std::size_t>(saturn::platform::AddressSpace::Vdp2)]
              << " scu="
              << mmioCounts[static_cast<std::size_t>(saturn::platform::AddressSpace::Scu)]
              << " scsp="
              << mmioCounts[static_cast<std::size_t>(saturn::platform::AddressSpace::Scsp)]
              << " cd="
              << mmioCounts[static_cast<std::size_t>(saturn::platform::AddressSpace::CdBlock)]
              << '\n';
    const auto& schedulerStats = scheduler.stats();
    std::cout << "  SCU scheduler: period=" << scheduler.vblankPeriod()
              << " blocks, vblank=" << schedulerStats.vblankEvents
              << ", vblank-out=" << schedulerStats.vblankOutEvents
              << ", hblank=" << schedulerStats.hblankEvents
              << ", timer0=" << schedulerStats.timer0Events
              << ", cd-subcode=" << schedulerStats.cdSubcodeEvents
              << ", delivered=" << schedulerStats.deliveredInterrupts
              << ", masked=" << schedulerStats.maskedEvents
              << ", dropped=" << schedulerStats.droppedEvents << '\n';
    std::cout << "  SCU interrupt state: status="
              << saturn::sh2::formatAddress(mmio.scuInterruptStatus())
              << ", mask=" << saturn::sh2::formatAddress(mmio.scuInterruptMask())
              << ", pending=" << saturn::sh2::formatAddress(mmio.scuInterruptPending())
              << '\n';
    const auto dmaDescriptors = mmio.scuDma().descriptors();
    std::cout << "  SCU DMA descriptors:";
    for (std::size_t channel = 0; channel < dmaDescriptors.size(); ++channel) {
        const auto& descriptor = dmaDescriptors[channel];
        std::cout << " D" << channel << "(" << saturn::sh2::formatAddress(descriptor.source)
                  << "->" << saturn::sh2::formatAddress(descriptor.destination)
                  << ", count=" << descriptor.count << ", add=" << descriptor.add
                  << ", mode=" << descriptor.mode
                  << (descriptor.enabled ? ", enabled" : ", idle")
                  << (descriptor.requested ? ", requested" : "") << ')';
    }
    if (const auto& transfer = mmio.scuDma().lastTransfer()) {
        std::cout << "\n  SCU DMA last transfer: channel=" << transfer->channel
                  << ", words=" << transfer->words << ", bytes=" << transfer->bytes
                  << ", requested=" << transfer->count << ", add=" << transfer->add
                  << ", mode=" << transfer->mode
                  << ", source=" << saturn::sh2::formatAddress(transfer->source)
                  << ", destination=" << saturn::sh2::formatAddress(transfer->destination)
                  << ", first-write="
                  << saturn::sh2::formatAddress(transfer->firstDestination)
                  << ", last-write="
                  << saturn::sh2::formatAddress(transfer->lastDestination)
                  << ", from-cd=" << (transfer->fromCd ? "yes" : "no")
                  << ", complete=" << (transfer->complete ? "yes" : "no");
    }
    std::cout << '\n';
    const auto& videoStats = mmio.video().vdp1CommandStats();
    const auto& lastVideoFrame = mmio.video().lastFrame();
    const auto nonBlackVideoPixels = static_cast<std::size_t>(std::count_if(
        lastVideoFrame.pixels.begin(), lastVideoFrame.pixels.end(),
        [](std::uint32_t pixel) { return pixel != 0xFF000000U; }));
    const auto vdp1Register = [&mmio](std::uint32_t offset) {
        return mmio.video().read(
            saturn::platform::VideoRuntime::kVdp1Registers + offset, 2U).value_or(0U);
    };
    const auto vdp2Register = [&mmio](std::uint32_t offset) {
        return mmio.video().read(
            saturn::platform::VideoRuntime::kVdp2Registers + offset, 2U).value_or(0U);
    };
    std::cout << "  VDP1 commands: " << videoStats.commands
              << " (sprites=" << videoStats.sprites
              << ", polygons=" << videoStats.polygons
              << ", lines=" << videoStats.lines << ")\n";
    std::cout << "  VDP1 registers: TVMR="
              << saturn::sh2::formatAddress(vdp1Register(0x00U))
              << " FBCR=" << saturn::sh2::formatAddress(vdp1Register(0x02U))
              << " PTMR=" << saturn::sh2::formatAddress(vdp1Register(0x04U))
              << " EDSR=" << saturn::sh2::formatAddress(vdp1Register(0x10U))
              << " LOPR=" << saturn::sh2::formatAddress(vdp1Register(0x12U))
              << " COPR=" << saturn::sh2::formatAddress(vdp1Register(0x14U)) << '\n';
    std::cout << "  video signal: display=" << (mmio.video().displayEnabled() ? "on" : "off")
              << ", non-black pixels=" << nonBlackVideoPixels << '\n';
    std::cout << "  VDP2 registers: TVMD=" << saturn::sh2::formatAddress(vdp2Register(0x000U))
              << " BGON=" << saturn::sh2::formatAddress(vdp2Register(0x020U))
              << " CHCTLA=" << saturn::sh2::formatAddress(vdp2Register(0x028U))
              << " BMPNA=" << saturn::sh2::formatAddress(vdp2Register(0x02CU))
              << " MPOFN=" << saturn::sh2::formatAddress(vdp2Register(0x03CU))
              << " BKTAU=" << saturn::sh2::formatAddress(vdp2Register(0x0ACU))
              << " BKTAL=" << saturn::sh2::formatAddress(vdp2Register(0x0AEU)) << '\n';
    const auto vdp1FramebufferWord = [&mmio](std::size_t index) {
        return mmio.video().read(
            saturn::platform::VideoRuntime::kVdp1Framebuffer +
                static_cast<saturn::core::Address>(index * 2U), 2U).value_or(0U);
    };
    const auto vdp1VramWord = [&mmio](std::size_t offset) {
        return mmio.video().read(
            saturn::platform::VideoRuntime::kVdp1Base +
                static_cast<saturn::core::Address>(offset), 2U).value_or(0U);
    };
    const auto vdp2VramWord = [&mmio](std::size_t offset) {
        return mmio.video().read(
            saturn::platform::VideoRuntime::kVdp2Base +
                static_cast<saturn::core::Address>(offset), 2U).value_or(0U);
    };
    std::size_t nonZeroVdp1Framebuffer{};
    for (std::size_t index = 0; index < saturn::platform::VideoRuntime::kVdp1FramebufferSize / 2U;
         ++index) {
        if (vdp1FramebufferWord(index) != 0U) ++nonZeroVdp1Framebuffer;
    }
    std::array<std::size_t, 8> nonZeroVdp1Offsets{};
    std::size_t nonZeroVdp1Samples{};
    for (std::size_t offset = 0; offset < saturn::platform::VideoRuntime::kVdp1VramSize;
         offset += 2U) {
        if (vdp1VramWord(offset) != 0U) {
            if (nonZeroVdp1Samples < nonZeroVdp1Offsets.size()) {
                nonZeroVdp1Offsets[nonZeroVdp1Samples++] = offset;
            }
        }
    }
    std::size_t nonZeroVdp2Vram{};
    std::array<std::size_t, 8> nonZeroVdp2Offsets{};
    std::size_t nonZeroVdp2Samples{};
    for (std::size_t offset = 0; offset < saturn::platform::VideoRuntime::kVdp2VramSize;
         offset += 2U) {
        if (vdp2VramWord(offset) != 0U) {
            ++nonZeroVdp2Vram;
            if (nonZeroVdp2Samples < nonZeroVdp2Offsets.size()) {
                nonZeroVdp2Offsets[nonZeroVdp2Samples++] = offset;
            }
        }
    }
    const auto nbg0Base = static_cast<std::size_t>(vdp2Register(0x03CU) & 0x7U) * 0x20000U;
    std::cout << "  video memory: VDP1 framebuffer nonzero=" << nonZeroVdp1Framebuffer
              << " VDP2 VRAM nonzero words=" << nonZeroVdp2Vram
              << " NBG0 base=" << saturn::sh2::formatAddress(
                     static_cast<std::uint32_t>(nbg0Base))
              << " samples=" << saturn::sh2::formatAddress(vdp2VramWord(nbg0Base))
              << ',' << saturn::sh2::formatAddress(vdp2VramWord(nbg0Base + 2U))
              << " VDP1cmd=" << saturn::sh2::formatAddress(vdp1VramWord(0U))
              << ',' << saturn::sh2::formatAddress(vdp1VramWord(2U))
              << ',' << saturn::sh2::formatAddress(vdp1VramWord(4U))
              << ',' << saturn::sh2::formatAddress(vdp1VramWord(6U))
              << ',' << saturn::sh2::formatAddress(vdp1VramWord(0x0CU))
              << ',' << saturn::sh2::formatAddress(vdp1VramWord(0x0EU))
              << " cmd20=" << saturn::sh2::formatAddress(vdp1VramWord(0x20U))
              << ',' << saturn::sh2::formatAddress(vdp1VramWord(0x24U))
              << ',' << saturn::sh2::formatAddress(vdp1VramWord(0x26U))
              << ',' << saturn::sh2::formatAddress(vdp1VramWord(0x2CU))
              << ',' << saturn::sh2::formatAddress(vdp1VramWord(0x2EU)) << '\n';
    std::cout << "  VDP2 nonzero offsets:";
    for (std::size_t index = 0; index < nonZeroVdp2Samples; ++index) {
        std::cout << ' ' << saturn::sh2::formatAddress(
            static_cast<std::uint32_t>(nonZeroVdp2Offsets[index]))
            << '=' << saturn::sh2::formatAddress(vdp2VramWord(nonZeroVdp2Offsets[index]));
    }
    std::cout << '\n';
    std::cout << "  VDP1 nonzero offsets:";
    for (std::size_t index = 0; index < nonZeroVdp1Samples; ++index) {
        std::cout << ' ' << saturn::sh2::formatAddress(
            static_cast<std::uint32_t>(nonZeroVdp1Offsets[index]))
            << '=' << saturn::sh2::formatAddress(vdp1VramWord(nonZeroVdp1Offsets[index]));
    }
    std::cout << '\n';
    std::cout << "  VDP1 DMA source: "
              << saturn::sh2::formatAddress(state.read32BE(0x06060594U)) << ','
              << saturn::sh2::formatAddress(state.read32BE(0x06060598U)) << ','
              << saturn::sh2::formatAddress(state.read32BE(0x0606059CU)) << ','
              << saturn::sh2::formatAddress(state.read32BE(0x060605A0U)) << '\n';
    std::size_t vdp2Writes{};
    const saturn::platform::MmioAccess* lastVdp2Write{};
    for (const auto& access : mmio.accesses()) {
        if (access.space == saturn::platform::AddressSpace::Vdp2 && access.write) {
            ++vdp2Writes;
            lastVdp2Write = &access;
        }
    }
    std::cout << "  VDP2 writes in retained window: " << vdp2Writes;
    if (lastVdp2Write != nullptr) {
        std::cout << " (last=" << saturn::sh2::formatAddress(lastVdp2Write->address)
                  << "=" << saturn::sh2::formatAddress(lastVdp2Write->value) << ')';
    }
    std::cout << '\n';
    std::size_t tvmdWrites{};
    std::size_t bgonWrites{};
    std::size_t wctlaWrites{};
    std::array<std::uint32_t, 8> tvmdHistory{};
    std::array<std::uint32_t, 8> bgonHistory{};
    std::array<std::uint32_t, 8> wctlaHistory{};
    for (const auto& access : mmio.accesses()) {
        if (access.space != saturn::platform::AddressSpace::Vdp2 || !access.write) {
            continue;
        }
        const auto offset = access.address & 0x000FFFFFU;
        if (offset == 0x80000U) {
            tvmdHistory[tvmdWrites % tvmdHistory.size()] = access.value;
            ++tvmdWrites;
        } else if (offset == 0x80020U) {
            bgonHistory[bgonWrites % bgonHistory.size()] = access.value;
            ++bgonWrites;
        } else if (offset == 0x800D0U) {
            wctlaHistory[wctlaWrites % wctlaHistory.size()] = access.value;
            ++wctlaWrites;
        }
    }
    std::cout << "  VDP2 control writes: TVMD=" << tvmdWrites << " [";
    for (std::size_t index = 0U; index < std::min(tvmdWrites, tvmdHistory.size()); ++index) {
        if (index != 0U) std::cout << ',';
        std::cout << saturn::sh2::formatAddress(tvmdHistory[index]);
    }
    std::cout << "] BGON=" << bgonWrites << " [";
    for (std::size_t index = 0U; index < std::min(bgonWrites, bgonHistory.size()); ++index) {
        if (index != 0U) std::cout << ',';
        std::cout << saturn::sh2::formatAddress(bgonHistory[index]);
    }
    std::cout << "] WCTLA=" << wctlaWrites << " [";
    for (std::size_t index = 0; index < std::min(wctlaWrites, wctlaHistory.size()); ++index) {
        if (index != 0U) std::cout << ',';
        std::cout << saturn::sh2::formatAddress(wctlaHistory[index]);
    }
    std::cout << "]\n";
    std::cout << "  CD boot RAM sample: [0606A054]="
              << saturn::sh2::formatAddress(state.read32BE(0x0606A054U))
              << " [0606A058]="
              << saturn::sh2::formatAddress(state.read32BE(0x0606A058U))
              << " [0606A05C]="
              << saturn::sh2::formatAddress(state.read32BE(0x0606A05CU))
              << '\n';
    std::cout << "  CD PVD bytes: [0606A0F6..A101]=";
    for (saturn::core::Address address = 0x0606A0F6U;
         address <= 0x0606A101U; ++address) {
        std::cout << std::hex << std::uppercase << std::setfill('0')
                  << std::setw(2) << static_cast<unsigned>(state.read8(address));
        if (address != 0x0606A101U) std::cout << ' ';
    }
    std::cout << std::dec << '\n';
    std::cout << "  GFS copy bytes: [06009794..97]="
              << saturn::sh2::formatAddress(state.read32BE(0x06009794U)) << '\n';
    const auto gfsDescriptorTable = state.read32BE(0x0606C1ACU);
    const auto gfsDescriptor98 = static_cast<saturn::core::Address>(
        gfsDescriptorTable + 0x98U);
    std::cout << "  GFS descriptor gate: table=[0606C1AC]="
              << saturn::sh2::formatAddress(gfsDescriptorTable)
              << " slot98(raw address)="
              << saturn::sh2::formatAddress(gfsDescriptor98) << '\n';
    std::cout << "  video-transfer literals: [06034BF0..BF8]="
              << saturn::sh2::formatAddress(state.read32BE(0x06034BF0U)) << ','
              << saturn::sh2::formatAddress(state.read32BE(0x06034BF4U)) << ','
              << saturn::sh2::formatAddress(state.read32BE(0x06034BF8U)) << '\n';
    std::cout << "  event RAM: [06051940]="
              << saturn::sh2::formatAddress(state.read8(0x06051940U))
              << " [06051944]="
              << saturn::sh2::formatAddress(state.read8(0x06051944U))
              << " [06051948]="
              << saturn::sh2::formatAddress(state.read8(0x06051948U))
              << " [060519A4]="
              << saturn::sh2::formatAddress(state.read8(0x060519A4U))
              << " [060519CA]="
              << saturn::sh2::formatAddress(memoryProbe
                     ? memoryProbe->waitEventValue()
                     : state.read8(0x060519CAU))
              << " [0605E7F6]="
              << saturn::sh2::formatAddress(state.read8(0x0605E7F6U))
              << " [0606C355]="
              << saturn::sh2::formatAddress(state.read8(0x0606C355U))
              << " [0606C498]="
              << saturn::sh2::formatAddress(state.read8(0x0606C498U))
              << '\n';
    std::cout << "  title async queue: index[0605E7F4]="
              << saturn::sh2::formatAddress(state.read8(0x0605E7F4U))
              << " count[0605E7F5]="
              << saturn::sh2::formatAddress(state.read8(0x0605E7F5U))
              << " busy[0605E7F6]="
              << saturn::sh2::formatAddress(state.read8(0x0605E7F6U))
              << " entries[0605E7B4..]=";
    for (std::size_t index = 0; index < 16U; ++index) {
        if (index != 0U) std::cout << ',';
        const auto entryAddress = static_cast<saturn::core::Address>(
            0x0605E7B4U + index * 4U);
        std::cout << saturn::sh2::formatAddress(state.read32BE(entryAddress));
    }
    std::cout << '\n';
    std::cout << "  interrupt tables: SCU[42]="
              << saturn::sh2::formatAddress(state.read32BE(0x06000900U + 0x42U * 4U))
              << " APP[42]="
              << saturn::sh2::formatAddress(state.read32BE(0x0606D0ECU + 0x42U * 4U))
              << " SCU[4D]="
              << saturn::sh2::formatAddress(state.read32BE(0x06000900U + 0x4DU * 4U))
              << " APP[4D]="
              << saturn::sh2::formatAddress(state.read32BE(0x0606D0ECU + 0x4DU * 4U))
              << " SCU[4B]="
              << saturn::sh2::formatAddress(state.read32BE(0x06000900U + 0x4BU * 4U))
              << " APP[4B]="
              << saturn::sh2::formatAddress(state.read32BE(0x0606D0ECU + 0x4BU * 4U))
              << " SCU[50]="
              << saturn::sh2::formatAddress(state.read32BE(0x06000900U + 0x50U * 4U))
              << " APP[50]="
              << saturn::sh2::formatAddress(state.read32BE(0x0606D0ECU + 0x50U * 4U))
              << " VBR[42]="
              << saturn::sh2::formatAddress(state.read32BE(
                     state.readState(saturn::sh2::stateSlot(
                         saturn::sh2::Register::VBR)) + 0x42U * 4U))
              << '\n';
    std::cout << "  SH-2 DMA regs: DMAOR="
              << saturn::sh2::formatAddress(state.read32BE(0xFFFFFFB0U))
              << " SAR0=" << saturn::sh2::formatAddress(state.read32BE(0xFFFFFF80U))
              << " DAR0=" << saturn::sh2::formatAddress(state.read32BE(0xFFFFFF84U))
              << " TCR0=" << saturn::sh2::formatAddress(state.read32BE(0xFFFFFF88U))
              << " CHCR0=" << saturn::sh2::formatAddress(state.read32BE(0xFFFFFF8CU))
              << " VCR0=" << saturn::sh2::formatAddress(state.read32BE(0xFFFFFFA0U))
              << '\n';
    const auto& scspStats = mmio.scsp().stats();
    std::cout << "  SCSP mailbox: ticks=" << scspStats.schedulerTicks
              << ", requests=" << scspStats.requests
              << ", completions=" << scspStats.completions
              << ", sequence=" << state.read16BE(saturn::platform::ScspRuntime::kSequence)
              << ", ready0=" << state.read16BE(saturn::platform::ScspRuntime::kReady0)
              << ", ready1=" << state.read16BE(saturn::platform::ScspRuntime::kReady1)
              << ", cd-shadow=0x" << std::hex << state.read16BE(0x0606D5B8U)
              << std::dec
              << '\n';
    std::cout << "  SCSP audio: frames=" << scspStats.audioFrames
              << ", samples=" << scspStats.audioSamples
              << ", active-voices=" << scspStats.activeVoices
              << ", pcm-starts=" << scspStats.pcmStarts
              << ", pcm-status-updates=" << scspStats.pcmStatusUpdates << '\n';
    if (mmio.scsp().driverCpuEnabled())
        std::cout << "  SCSP driver: cycles=" << scspStats.driverCycles << ", pc="
                  << saturn::sh2::formatAddress(scspStats.driverPc)
                  << ", resets=" << scspStats.driverResets << ", register-writes=" << scspStats.driverRegisterWrites
                  << ", key-ons=" << scspStats.slotKeyOns << ", key-offs=" << scspStats.slotKeyOffs
                  << ", slot-mask=" << saturn::sh2::formatAddress(scspStats.slotMask)
                  << ", unmapped=" << scspStats.driverUnmappedReads << '/' << scspStats.driverUnmappedWrites
                  << ", unsupported-source=" << scspStats.unsupportedSourceSamples
                  << ", noise=" << scspStats.noiseSamples
                  << ", pitch-lfo=" << scspStats.pitchLfoSamples
                  << ", amplitude-lfo=" << scspStats.amplitudeLfoSamples
                  << ", fm=" << scspStats.fmSamples
                  << ", dsp-steps=" << scspStats.dspSteps
                  << ", dsp-send=" << scspStats.dspSendSamples
                  << ", dsp-effect=" << scspStats.dspEffectSamples
                  << ", audio-blocks=" << scspStats.audioBlockTicks
                  << ", out-of-range=" << scspStats.slotOutOfRangeSamples
                  << ", sdk-error-bits=" << saturn::sh2::formatAddress(mmio.scsp().read(
                        saturn::platform::ScspRuntime::kRamBase + 0x420U, 4U).value_or(0U)) << '\n';
    // Keep the final SCSP host/status bytes visible when an FMV reaches its
    // last descriptor but the guest does not return to the title loop.  The
    // guest compares host+0xA0 status bytes against its cache at 0x0606E608;
    // this distinguishes a stalled consumer from a producer that never sees
    // the PCM position advance.
    const auto scspHost = state.read32BE(0x25A00404U);
    std::cout << "  SCSP commands:";
    for (std::size_t command = 1; command < mmio.scsp().stats().soundCommands.size(); ++command) {
        const auto count = mmio.scsp().stats().soundCommands[command];
        if (count != 0U) std::cout << ' ' << saturn::sh2::formatAddress(
            static_cast<std::uint32_t>(command)) << '=' << count;
    }
    std::cout << " rejected-PCM-starts=" << mmio.scsp().stats().pcmRejectedStarts
              << " last-PCM-start=";
    for (const auto word : mmio.scsp().stats().lastPcmStart) {
        std::cout << saturn::sh2::formatAddress(word) << ',';
    }
    std::cout << '\n';
    std::cout << "  SCSP PCM host=" << saturn::sh2::formatAddress(scspHost)
              << " status=";
    for (std::uint32_t index = 0U; index < 8U; ++index) {
        std::cout << saturn::sh2::formatAddress(
            mmio.read(0x25A007A0U + index * 2U, 1U).value_or(0U)) << ',';
    }
    std::cout << " cache=";
    for (std::uint32_t index = 0U; index < 8U; ++index) {
        std::cout << saturn::sh2::formatAddress(
            state.read8(0x0606E608U + index)) << ',';
    }
    std::cout << '\n';
    bool audioWriteFailure{};
    if (arguments.audioOutput) {
        audioWriteFailure = !writeAudioWav(*arguments.audioOutput, capturedAudio,
                                           saturn::platform::ScspRuntime::kAudioSampleRate,
                                           static_cast<std::uint16_t>(
                                               saturn::platform::ScspRuntime::kAudioChannels));
        std::cout << "  audio output: " << arguments.audioOutput->string()
                  << (audioWriteFailure ? " (write-error)" : "") << '\n';
    }
    // Sound command packets seen on the host mailbox, in order. Each row is a
    // cue the game requested (0x01 starts a sequence, 0x08 loads a map, ...);
    // the step column aligns it with scripted input and the captured audio.
    if (arguments.scspRequestsOutput) {
        std::ofstream requests(*arguments.scspRequestsOutput, std::ios::trunc);
        const auto& sound = mmio.scsp().stats();
        if (!requests) {
            std::cout << "  SCSP requests: write-error\n";
        } else {
            requests << "step,command,control,bank,sequence\n";
            for (const auto& request : sound.lastSoundRequests)
                requests << request.schedulerTick << ',' << static_cast<unsigned>(request.command)
                         << ',' << static_cast<unsigned>(request.control)
                         << ',' << static_cast<unsigned>(request.bank)
                         << ',' << static_cast<unsigned>(request.sequence) << '\n';
            std::cout << "  SCSP sound requests: " << sound.soundRequests << " total ("
                      << sound.sequenceRequests << " sequence starts), "
                      << sound.lastSoundRequests.size() << " recorded to "
                      << arguments.scspRequestsOutput->string() << '\n';
        }
    }
    if (execution.status == saturn::sir::ProgramStatus::UnresolvedTarget) {
        std::cout << "  unresolved target: "
                  << saturn::sh2::formatAddress(execution.unresolvedTarget) << '\n';
    }
    if (arguments.trace) {
        std::cout << "\nRecent block trace:\n";
        const auto recentShown = std::min(recentBlockCount, recentBlocks.size());
        const auto recentStart = recentBlockCount > recentBlocks.size() ?
            recentBlockCount % recentBlocks.size() : 0U;
        for (std::size_t index = 0; index < recentShown; ++index) {
            const auto& [address, step] =
                recentBlocks[(recentStart + index) % recentBlocks.size()];
            std::cout << "  step " << step << ' '
                      << saturn::sh2::formatAddress(address) << '\n';
        }

        std::cout << "\nBoot service trace:\n";
        if (bootServices.calls().empty()) {
            std::cout << "  none\n";
        }
        for (const auto& call : bootServices.calls()) {
            std::cout << "  " << call.service.name
                      << " target=" << saturn::sh2::formatAddress(call.service.targetAddress);
            printContext(call.context);
            std::cout << '\n';
        }

        std::cout << "\nMMIO trace:\n";
        if (mmio.accesses().empty()) {
            std::cout << "  none\n";
        }
        constexpr std::size_t kTraceLimit = 256;
        const auto shown = std::min(mmio.accesses().size(), kTraceLimit);
        for (std::size_t index = 0; index < shown; ++index) {
            const auto& access = mmio.accesses()[index];
            std::cout << "  " << (access.write ? "write" : "read")
                      << static_cast<unsigned>(access.width) * 8U
                      << ' ' << saturn::platform::name(access.space)
                      << '[' << saturn::sh2::formatAddress(access.address) << "]="
                      << saturn::sh2::formatAddress(access.value);
            printContext(access.context);
            std::cout << '\n';
        }
        if (shown < mmio.accesses().size()) {
            std::cout << "  ... " << (mmio.accesses().size() - shown)
                      << " additional accesses omitted\n";
        }

        const auto printRecentSpace = [&mmio](saturn::platform::AddressSpace space,
                                               std::string_view label,
                                               std::size_t limit) {
            std::vector<const saturn::platform::MmioAccess*> recent;
            recent.reserve(limit);
            for (auto iterator = mmio.accesses().rbegin();
                 iterator != mmio.accesses().rend() && recent.size() < limit;
                 ++iterator) {
                if (iterator->space == space) recent.push_back(&*iterator);
            }
            std::cout << "\nRecent " << label << " trace:\n";
            if (recent.empty()) std::cout << "  none\n";
            for (auto iterator = recent.rbegin(); iterator != recent.rend(); ++iterator) {
                const auto& access = **iterator;
                std::cout << "  " << (access.write ? "write" : "read")
                          << static_cast<unsigned>(access.width) * 8U
                          << " [" << saturn::sh2::formatAddress(access.address) << "]="
                          << saturn::sh2::formatAddress(access.value);
                printContext(access.context);
                std::cout << '\n';
            }
        };
        printRecentSpace(saturn::platform::AddressSpace::CdBlock,
                         "CD Block", 256U);
        std::cout << "\nCD command issue trace:\n";
        std::array<std::uint16_t, 4> commandRegisters{};
        std::size_t commandTraceCount{};
        constexpr std::size_t kCommandTraceLimit = 256U;
        for (const auto& access : mmio.accesses()) {
            const auto offset = access.address & 0x000FFFFFU;
            if (!access.write || access.space != saturn::platform::AddressSpace::CdBlock) {
                continue;
            }
            if (offset == 0x90018U || offset == 0x9001AU) {
                commandRegisters[0] = static_cast<std::uint16_t>(access.value);
            } else if (offset == 0x9001CU || offset == 0x9001EU) {
                commandRegisters[1] = static_cast<std::uint16_t>(access.value);
            } else if (offset == 0x90020U || offset == 0x90022U) {
                commandRegisters[2] = static_cast<std::uint16_t>(access.value);
            } else if (offset != 0x90024U && offset != 0x90026U) {
                continue;
            } else {
                if (commandTraceCount >= kCommandTraceLimit) continue;
                std::cout << "  " << std::hex << commandRegisters[0] << ' '
                          << commandRegisters[1] << ' ' << commandRegisters[2]
                          << ' ' << static_cast<std::uint16_t>(access.value)
                          << std::dec;
                printContext(access.context);
                std::cout << '\n';
                ++commandTraceCount;
            }
        }
        printRecentSpace(saturn::platform::AddressSpace::Vdp1,
                         "VDP1", 128U);
        printRecentSpace(saturn::platform::AddressSpace::Scsp,
                         "SCSP", 256U);
        if (memoryProbe) {
            std::cout << "\nCD/GFS RAM probe (PVD 0606A000..A11F + copy 06009780..97AF):\n";
            for (const auto& record : memoryProbe->records()) {
                std::cout << "  " << (record.write ? "write" : "read")
                          << static_cast<unsigned>(record.width) * 8U
                          << " [" << saturn::sh2::formatAddress(record.address)
                          << "]=" << saturn::sh2::formatAddress(record.value);
                printContext(record.context);
                std::cout << '\n';
            }
        }
    }
    if (memoryProbe) {
        std::cout << "\nRecent nonzero CD commands: total="
                  << memoryProbe->cdCommandRecordCount() << '\n';
        const auto& cdCommands = memoryProbe->cdCommandRecords();
        const auto cdCommandsShown = std::min<std::size_t>(32U, cdCommands.size());
        const auto cdCommandsStart = cdCommands.empty() ? 0U :
            (memoryProbe->cdCommandRecordCount() - cdCommandsShown) % cdCommands.size();
        for (std::size_t index = 0U; index < cdCommandsShown; ++index) {
            const auto& record = cdCommands[
                (cdCommandsStart + index) % cdCommands.size()];
            std::cout << "  command=" << saturn::sh2::formatAddress(record.value)
                      << " issuer-return="
                      << saturn::sh2::formatAddress(record.address);
            printContext(record.context);
            std::cout << '\n';
        }

        std::cout << "\nCD PLAY command issuers:\n";
        for (const auto& record : memoryProbe->cdPlayCommandRecords()) {
            std::cout << "  issuer-return="
                      << saturn::sh2::formatAddress(record.command.address)
                      << " command="
                      << saturn::sh2::formatAddress(record.command.value)
                      << " title-state=" << record.titleStateIndex
                      << " title-callback="
                      << saturn::sh2::formatAddress(record.titleStateTarget);
            printContext(record.command.context);
            std::cout << " play-count=" << record.playCommands << " play-registers=";
            for (const auto value : record.playRegisters) {
                std::cout << saturn::sh2::formatAddress(value) << ',';
            }
            std::cout << " stack=";
            for (const auto value : record.stack) {
                std::cout << saturn::sh2::formatAddress(value) << ',';
            }
            std::cout << '\n';
        }

        std::cout << "\nTitle-work CD sector requests (GET DATA/GET+DELETE): total="
                  << memoryProbe->cdSectorRequestRecordCount() << '\n';
        if (memoryProbe->cdSectorRequestRecords().empty()) {
            std::cout << "  none observed after the title queue request\n";
        }
        for (const auto& record : memoryProbe->cdSectorRequestRecords()) {
            std::cout << "  command="
                      << saturn::sh2::formatAddress(record.command.value)
                      << " issuer-return="
                      << saturn::sh2::formatAddress(record.command.address);
            printContext(record.command.context);
            std::cout << " transfer-sectors=" << record.transferSectorRequests
                      << " last-transfer=(lba=" << record.transferLba
                      << ",offset=" << record.transferOffset
                      << ",count=" << record.transferCount
                      << ",partition=" << static_cast<unsigned>(record.transferPartition)
                      << ",instruction=0x" << std::hex
                      << static_cast<unsigned>(record.transferInstruction) << std::dec
                      << ") stack=";
            for (const auto value : record.stack) {
                std::cout << saturn::sh2::formatAddress(value) << ',';
            }
            std::cout << '\n';
        }

        std::cout << "\nTitle async queue writes: total="
                  << memoryProbe->titleQueueWriteCount() << '\n';
        for (const auto& record : memoryProbe->titleQueueWrites()) {
            std::cout << "  [" << saturn::sh2::formatAddress(record.address)
                      << "]=" << saturn::sh2::formatAddress(record.value)
                      << " width=" << static_cast<unsigned>(record.width);
            printContext(record.context);
            std::cout << '\n';
        }

        std::cout << "\nTitle GFS progress-state reads: total="
                  << memoryProbe->titleProgressStateReadCount()
                  << " final=(0x06053350="
                  << saturn::sh2::formatAddress(memoryProbe->ram32(0x06053350U))
                  << ",0x06053354="
                  << saturn::sh2::formatAddress(memoryProbe->ram32(0x06053354U))
                  << ")\n";
        const auto& progressReads = memoryProbe->titleProgressStateReads();
        const auto progressReadsShown = std::min<std::size_t>(64U, progressReads.size());
        const auto progressReadsStart = progressReads.empty() ? 0U :
            (memoryProbe->titleProgressStateReadCount() - progressReadsShown) %
                progressReads.size();
        for (std::size_t index = 0U; index < progressReadsShown; ++index) {
            const auto& record = progressReads[
                (progressReadsStart + index) % progressReads.size()];
            std::cout << "  read[" << saturn::sh2::formatAddress(record.address)
                      << "]=" << saturn::sh2::formatAddress(record.value)
                      << " width=" << static_cast<unsigned>(record.width);
            printContext(record.context);
            std::cout << '\n';
        }

        std::cout << "\nTitle GFS progress-state writes: total="
                  << memoryProbe->titleProgressStateWriteCount() << '\n';
        for (const auto& record : memoryProbe->titleProgressStateWrites()) {
            std::cout << "  write[" << saturn::sh2::formatAddress(record.address)
                      << "]=" << saturn::sh2::formatAddress(record.value)
                      << " width=" << static_cast<unsigned>(record.width);
            printContext(record.context);
            std::cout << '\n';
        }

        std::cout << "\nTitle resolver and worker flow: total="
                  << memoryProbe->resolverFlowRecordCount() << '\n';
        for (const auto& record : memoryProbe->resolverFlowRecords()) {
            std::cout << "  block=" << saturn::sh2::formatAddress(record.block)
                      << " caller=" << saturn::sh2::formatAddress(record.caller)
                      << " step=" << record.step << " regs=";
            for (std::size_t index = 0; index < record.regs.size(); ++index) {
                std::cout << 'r' << index << '='
                          << saturn::sh2::formatAddress(record.regs[index]) << ',';
            }
            if (!record.lookupKey.empty()) {
                std::cout << " lookup-key=" << std::quoted(record.lookupKey);
            }
            std::cout << '\n';
        }

        std::cout << "\nStage archive open flow: total="
                  << memoryProbe->stageArchiveFlowRecordCount() << '\n';
        for (const auto& record : memoryProbe->stageArchiveFlowRecords()) {
            std::cout << "  block=" << saturn::sh2::formatAddress(record.block)
                      << " caller=" << saturn::sh2::formatAddress(record.caller)
                      << " step=" << record.step;
            if (!record.lookupKey.empty()) {
                const auto label = record.block == 0x002719E8U
                                       ? " template="
                                       : record.block == 0x0603AD4AU
                                             ? " resolved-name="
                                             : " name=";
                std::cout << label << std::quoted(record.lookupKey);
            }
            if (record.overlayState.has_value()) {
                std::cout << " overlay-state="
                          << static_cast<unsigned>(*record.overlayState);
            }
            if (record.firstNameIndex.has_value() &&
                record.secondNameIndex.has_value()) {
                std::cout << " name-indices=("
                          << static_cast<unsigned>(*record.firstNameIndex) << ','
                          << static_cast<unsigned>(*record.secondNameIndex) << ')';
            }
            std::cout << " r4=" << saturn::sh2::formatAddress(record.regs[4])
                      << " r12=" << saturn::sh2::formatAddress(record.regs[12])
                      << " r13=" << saturn::sh2::formatAddress(record.regs[13])
                      << " r14=" << saturn::sh2::formatAddress(record.regs[14])
                      << '\n';
        }

        std::cout << "\nTitle GFS handle setup and extension gate:\n";
        for (const auto& record : memoryProbe->titleGfsSetupRecords()) {
            std::cout << "  block=" << saturn::sh2::formatAddress(record.block)
                      << " caller=" << saturn::sh2::formatAddress(record.caller)
                      << " step=" << record.step
                      << " handle=" << saturn::sh2::formatAddress(record.handle)
                      << " arg-r4=" << saturn::sh2::formatAddress(record.handleArgument)
                      << " state=" << saturn::sh2::formatAddress(record.handleState)
                      << " handle+0x28="
                      << saturn::sh2::formatAddress(record.handleWordAt28)
                      << " ring=("
                      << saturn::sh2::formatAddress(record.ringFields[0]) << ','
                      << saturn::sh2::formatAddress(record.ringFields[1]) << ','
                      << saturn::sh2::formatAddress(record.ringFields[2])
                      << ") regs=";
            for (std::size_t index = 0; index < record.regs.size(); ++index) {
                std::cout << 'r' << index << '='
                          << saturn::sh2::formatAddress(record.regs[index]) << ',';
            }
            std::cout << '\n';
        }
        std::cout << "Title GFS handle+0x28 writes: total="
                  << memoryProbe->titleGfsHookWriteCount()
                  << " final="
                  << saturn::sh2::formatAddress(memoryProbe->ram32(0x0606A87CU))
                  << '\n';
        for (const auto& record : memoryProbe->titleGfsHookWrites()) {
            std::cout << "  write[" << saturn::sh2::formatAddress(record.address)
                      << "]=" << saturn::sh2::formatAddress(record.value)
                      << " width=" << static_cast<unsigned>(record.width);
            printContext(record.context);
            std::cout << '\n';
        }
        std::cout << "Title GFS handle member writes after queue: total="
                  << memoryProbe->titleGfsMemberWriteCount() << '\n';
        for (const auto& record : memoryProbe->titleGfsMemberWrites()) {
            std::cout << "  write[" << saturn::sh2::formatAddress(record.address)
                      << "]=" << saturn::sh2::formatAddress(record.value)
                      << " width=" << static_cast<unsigned>(record.width);
            printContext(record.context);
            std::cout << '\n';
        }

        std::cout << "\nFocused dispatcher/callback hits:\n";
        const auto& targets = LaunchMemoryProbe::targetAddresses();
        const auto& hits = memoryProbe->targetHits();
        for (std::size_t index = 0; index < targets.size(); ++index) {
            std::cout << "  " << saturn::sh2::formatAddress(targets[index])
                      << ": " << hits[index] << '\n';
        }

        std::cout << "\nApplication dispatcher function-table calls:\n";
        for (const auto& record : memoryProbe->dispatcherDispatches()) {
            std::cout << "  index=" << record.index
                      << " table=" << saturn::sh2::formatAddress(record.table)
                      << " slot=" << saturn::sh2::formatAddress(record.slot)
                      << " target=" << saturn::sh2::formatAddress(record.target)
                      << " hits=" << record.hits
                      << " first-step=" << record.firstStep
                      << " last-step=" << record.lastStep
                      << " caller=" << saturn::sh2::formatAddress(record.caller);
            printContext(record.context);
            std::cout << '\n';
        }

        std::cout << "\nRockman object callback table dispatches:\n";
        for (const auto& record : memoryProbe->objectCallbackDispatches()) {
            std::cout << "  caller=" << saturn::sh2::formatAddress(record.caller)
                      << " callback=" << saturn::sh2::formatAddress(record.callback)
                      << " hits=" << record.hits
                      << " first-step=" << record.firstStep
                      << " last-step=" << record.lastStep
                      << " object=" << saturn::sh2::formatAddress(record.object)
                      << " id[r4+1]=0x" << std::hex
                      << static_cast<unsigned>(record.objectId)
                      << " slot=" << saturn::sh2::formatAddress(record.slotByObjectId)
                      << " table-target="
                      << saturn::sh2::formatAddress(record.targetByObjectId)
                      << " id[r9+1]=0x" << static_cast<unsigned>(record.r9Id)
                      << " slot=" << saturn::sh2::formatAddress(record.slotByR9Id)
                      << " table-target="
                      << saturn::sh2::formatAddress(record.targetByR9Id)
                      << " r3=" << saturn::sh2::formatAddress(record.r3Target)
                      << std::dec << " regs=";
            for (std::size_t index = 0; index < record.regs.size(); ++index) {
                std::cout << 'r' << index << '='
                          << saturn::sh2::formatAddress(record.regs[index]) << ',';
            }
            std::cout << '\n';
        }

        std::cout << "\nTitle callback table visits:\n";
        for (const auto& record : memoryProbe->titleCallbackStateRecords()) {
            std::cout << "  index=" << record.index
                      << " target=" << saturn::sh2::formatAddress(record.target)
                      << " hits=" << record.hits
                      << " first-step=" << record.firstStep
                      << " last-step=" << record.lastStep
                      << " slot=" << saturn::sh2::formatAddress(record.slot);
            printContext(record.context);
            std::cout << '\n';
        }
        const auto titleStateIndex = memoryProbe->ram32(0x060519CCU);
        std::cout << "  final-state-index=" << titleStateIndex
                  << " final-state-target=";
        if (titleStateIndex < 9U) {
            std::cout << saturn::sh2::formatAddress(
                memoryProbe->ram32(static_cast<saturn::core::Address>(
                    0x060460E8U + titleStateIndex * 4U)));
        } else {
            std::cout << "out-of-table";
        }
        std::cout << '\n';

        std::cout << "\nFocused foreground routine snapshots:\n";
        for (const auto& record : memoryProbe->foregroundProbeRecords()) {
            std::cout << "  block=" << saturn::sh2::formatAddress(record.block)
                      << " hits=" << record.hits
                      << " first-step=" << record.firstStep
                      << " last-step=" << record.lastStep << " regs=";
            for (std::size_t index = 0; index < record.regs.size(); ++index) {
                std::cout << 'r' << index << '='
                          << saturn::sh2::formatAddress(record.regs[index]) << ',';
            }
            std::cout << " *r4/*r6/*r9/*r13=";
            for (const auto value : record.pointedWords) {
                std::cout << saturn::sh2::formatAddress(value) << ',';
            }
            std::cout << " stack=";
            for (const auto value : record.stackWords) {
                std::cout << saturn::sh2::formatAddress(value) << ',';
            }
            printContext(record.context);
            std::cout << '\n';
        }

        std::cout << "\nGFS update checkpoints:\n";
        const auto& gfsAddresses = LaunchMemoryProbe::gfsCheckpointAddresses();
        const auto& gfsHits = memoryProbe->gfsCheckpointHits();
        const auto& gfsRecords = memoryProbe->lastGfsCheckpoints();
        for (std::size_t index = 0; index < gfsAddresses.size(); ++index) {
            std::cout << "  " << saturn::sh2::formatAddress(gfsAddresses[index])
                      << ": hits=" << gfsHits[index];
            if (gfsRecords[index]) {
                const auto& record = *gfsRecords[index];
                std::cout << " object=" << saturn::sh2::formatAddress(record.object)
                          << " stack=";
                for (const auto value : record.stack) {
                    std::cout << saturn::sh2::formatAddress(value) << ',';
                }
                std::cout << " fields[188..1A4]="
                          << saturn::sh2::formatAddress(record.source) << ','
                          << saturn::sh2::formatAddress(record.active) << ','
                          << saturn::sh2::formatAddress(record.submitted) << ','
                          << saturn::sh2::formatAddress(record.consumed) << ','
                          << saturn::sh2::formatAddress(record.cursor) << ','
                          << saturn::sh2::formatAddress(record.target)
                          << " stream=" << saturn::sh2::formatAddress(record.stream)
                          << " stream[84..112]=";
                for (const auto value : record.streamFields) {
                    std::cout << saturn::sh2::formatAddress(value) << ',';
                }
                printContext(record.context);
            }
            std::cout << '\n';
        }

        std::cout << "\nGFS ring consumer advances: total="
                  << memoryProbe->gfsProducerRecordCount() << '\n';
        for (const auto& record : memoryProbe->gfsProducerRecords()) {
            std::cout << "  caller=" << saturn::sh2::formatAddress(record.caller)
                      << " wrapper=" << saturn::sh2::formatAddress(record.wrapper)
                      << " consumed=" << saturn::sh2::formatAddress(record.produced)
                      << " object=" << saturn::sh2::formatAddress(record.object)
                      << " stream=" << saturn::sh2::formatAddress(record.stream)
                      << " fields[84..112]=";
            for (const auto value : record.streamFields) {
                std::cout << saturn::sh2::formatAddress(value) << ',';
            }
            printContext(record.context);
            std::cout << '\n';
        }

        if (memoryProbe->lastFirstProducerExit()) {
            std::cout << "\nLast audio-consumer exit: stack[0..12]=";
            for (const auto value : memoryProbe->firstProducerExitStack()) {
                std::cout << saturn::sh2::formatAddress(value) << ',';
            }
            printContext(*memoryProbe->lastFirstProducerExit());
            std::cout << '\n';
        }
        std::cout << "\nAudio output controller="
                  << saturn::sh2::formatAddress(memoryProbe->audioController())
                  << " fields[0x80..0xAC]=";
        for (std::uint32_t offset = 0x80U; offset < 0xB0U; offset += 4U) {
            std::cout << saturn::sh2::formatAddress(memoryProbe->ram32(
                memoryProbe->audioController() + offset)) << ',';
        }
        std::cout << " ring-extra[0x180..0x1A4]=";
        for (std::uint32_t offset = 0x180U; offset <= 0x1A4U; offset += 4U) {
            std::cout << saturn::sh2::formatAddress(memoryProbe->ram32(
                memoryProbe->audioController() + offset)) << ',';
        }
        std::cout << " object-extra[0x180..0x1A4]=";
        const auto audioObject = static_cast<saturn::core::Address>(
            memoryProbe->audioController() - 56U);
        for (std::uint32_t offset = 0x180U; offset <= 0x1A4U; offset += 4U) {
            std::cout << saturn::sh2::formatAddress(memoryProbe->ram32(
                audioObject + offset)) << ',';
        }
        std::cout << "\nAudio cursor changes: total="
                  << memoryProbe->audioCursorWriteCount() << '\n';
        for (const auto& record : memoryProbe->audioCursorWrites()) {
            std::cout << "  address=" << saturn::sh2::formatAddress(record.address)
                      << " value=" << saturn::sh2::formatAddress(record.value);
            printContext(record.context);
            std::cout << '\n';
        }
        std::cout << "Sound commands: total=" << memoryProbe->soundCommandCount() << '\n';
        for (const auto& record : memoryProbe->soundCommands()) {
            std::cout << "  packet=" << saturn::sh2::formatAddress(record.packet) << " words=";
            for (const auto word : record.words) {
                std::cout << saturn::sh2::formatAddress(word) << ',';
            }
            printContext(record.context);
            std::cout << '\n';
        }
        std::cout << "Audio status poll/dispatch/advance/read counts=";
        for (const auto count : memoryProbe->audioStatusCounts()) {
            std::cout << count << ',';
        }
        if (memoryProbe->lastAudioStatusRead()) {
            const auto& record = *memoryProbe->lastAudioStatusRead();
            std::cout << " last-address=" << saturn::sh2::formatAddress(record.address)
                      << " value=" << saturn::sh2::formatAddress(record.value);
            printContext(record.context);
        }
        std::cout << '\n';
        std::cout << "Audio status cache writes="
                  << memoryProbe->audioStatusCacheWriteCount() << '\n';
        for (const auto& record : memoryProbe->audioStatusCacheWrites()) {
            std::cout << "  address=" << saturn::sh2::formatAddress(record.address)
                      << " value=" << saturn::sh2::formatAddress(record.value);
            printContext(record.context);
            std::cout << '\n';
        }
        const auto& producerPath = memoryProbe->gfsProducerPathRecords();
        const auto producerPathCount = memoryProbe->gfsProducerPathRecordCount();
        std::cout << "\nGFS producer causal path: total=" << producerPathCount << '\n';
        if (!producerPath.empty()) {
            const auto producerPathStart = producerPathCount > producerPath.size()
                ? producerPathCount % producerPath.size()
                : 0U;
            const auto pathRecord = [&](std::size_t logicalIndex)
                -> const GfsProducerPathRecord& {
                return producerPath[(producerPathStart + logicalIndex) %
                                    producerPath.size()];
            };
            const auto printPathRecord = [&](const GfsProducerPathRecord& record) {
                const auto available = static_cast<std::uint32_t>(
                    record.streamFields[7] + record.streamFields[1] -
                    record.streamFields[0]);
                std::cout << "  block=" << saturn::sh2::formatAddress(record.block)
                          << " from=" << saturn::sh2::formatAddress(record.caller)
                          << " step=" << record.context.step
                          << " pubs=" << record.producerAdvances
                          << " cmds=" << record.cdCommands
                          << " r0=" << saturn::sh2::formatAddress(record.result)
                          << " ctl=" << saturn::sh2::formatAddress(record.controller)
                          << " state/index/count="
                          << saturn::sh2::formatAddress(record.controllerFields[0]) << '/'
                          << record.controllerFields[1] << '/'
                          << record.descriptorCount
                          << " ctl[20,32,36,44]="
                          << saturn::sh2::formatAddress(record.controllerFields[2]) << ','
                          << saturn::sh2::formatAddress(record.controllerFields[3]) << ','
                          << saturn::sh2::formatAddress(record.controllerFields[4]) << ','
                          << saturn::sh2::formatAddress(record.controllerFields[5])
                          << " transfer="
                          << saturn::sh2::formatAddress(record.controllerFields[6])
                        << " desc=" << saturn::sh2::formatAddress(record.descriptor)
                          << " desc[0..12]=";
                for (const auto value : record.descriptorFields) {
                    std::cout << saturn::sh2::formatAddress(value) << ',';
                }
                std::cout << " ring=" << saturn::sh2::formatAddress(record.stream)
                          << " ring[0,4,12,20,112]=";
                for (const auto value : record.ringFields) {
                    std::cout << saturn::sh2::formatAddress(value) << ',';
                }
                std::cout
                          << " alloc_base="
                          << saturn::sh2::formatAddress(record.ringAllocationBase)
                          << " alloc_slot="
                          << saturn::sh2::formatAddress(record.ringAllocationSlot)
                          << " available=" << saturn::sh2::formatAddress(available)
                          << " poll="
                          << saturn::sh2::formatAddress(record.pollOutputs[0]) << ','
                          << saturn::sh2::formatAddress(record.pollOutputs[1])
                          << " transfer[8C,9C,A0,120,124,obj30]=";
                for (const auto value : record.transferFields) {
                    std::cout << saturn::sh2::formatAddress(value) << ',';
                }
                std::cout
                          << '\n';
            };

            std::optional<std::size_t> pendingFirstPath;
            std::optional<std::size_t> pendingSubsequentPath;
            std::optional<std::size_t> lastSuccessfulReturn;
            for (std::size_t index = 0U; index < producerPath.size(); ++index) {
                const auto& record = pathRecord(index);
                if (record.block == 0x0603FC90U) {
                    pendingFirstPath = index;
                } else if (record.block == 0x0604074CU) {
                    pendingSubsequentPath = index;
                } else if (record.block == 0x0604073AU && pendingFirstPath) {
                    if (record.producerAdvances >
                        pathRecord(*pendingFirstPath).producerAdvances) {
                        lastSuccessfulReturn = index;
                    }
                    pendingFirstPath.reset();
                } else if (record.block == 0x06040762U && pendingSubsequentPath) {
                    if (record.producerAdvances >
                        pathRecord(*pendingSubsequentPath).producerAdvances) {
                        lastSuccessfulReturn = index;
                    }
                    pendingSubsequentPath.reset();
                }
            }
            std::optional<std::size_t> firstStalledReturn;
            if (lastSuccessfulReturn) {
                for (std::size_t index = *lastSuccessfulReturn + 1U;
                     index < producerPath.size(); ++index) {
                    if (pathRecord(index).block == 0x0604073AU ||
                        pathRecord(index).block == 0x06040762U) {
                        firstStalledReturn = index;
                        break;
                    }
                }
            }
            const auto printInvocation = [&](std::string_view label,
                                             std::size_t returnIndex) {
                auto begin = returnIndex;
                const auto invocationStart = pathRecord(returnIndex).block == 0x06040762U
                    ? 0x0604074CU
                    : 0x06040724U;
                while (begin != 0U &&
                       pathRecord(begin).block != invocationStart) {
                    --begin;
                }
                auto end = returnIndex;
                const auto invocationEnd = pathRecord(returnIndex).block == 0x06040762U
                    ? 0x06040762U
                    : 0x06040744U;
                while (end + 1U < producerPath.size() &&
                       pathRecord(end).block != invocationEnd) {
                    ++end;
                }
                std::cout << "  " << label << ":\n";
                for (auto index = begin; index <= end; ++index) {
                    printPathRecord(pathRecord(index));
                }
            };
            if (lastSuccessfulReturn) {
                printInvocation("last publication-producing invocation",
                                *lastSuccessfulReturn);
            } else {
                std::cout << "  no publication-producing invocation found\n";
            }
            if (firstStalledReturn) {
                printInvocation("first following non-publishing invocation",
                                *firstStalledReturn);
            }
            std::cout << "  final path events:\n";
            const auto tailStart = producerPath.size() > 12U
                ? producerPath.size() - 12U
                : 0U;
            for (auto index = tailStart; index < producerPath.size(); ++index) {
                printPathRecord(pathRecord(index));
            }
        }

        std::cout << "\nGFS transfer polls: total="
                  << memoryProbe->gfsTransferPollCount() << '\n';
        const auto& transferPolls = memoryProbe->gfsTransferPolls();
        const auto transferPollStart = memoryProbe->gfsTransferPollCount() > 24U
            ? memoryProbe->gfsTransferPollCount() - 24U : 0U;
        for (std::size_t logical = transferPollStart;
             logical < memoryProbe->gfsTransferPollCount(); ++logical) {
            const auto& record = transferPolls[
                logical % transferPolls.size()];
            std::cout << "  block=" << saturn::sh2::formatAddress(record.block)
                      << " step=" << record.context.step
                      << " wrapper=" << saturn::sh2::formatAddress(record.wrapper)
                      << " object=" << saturn::sh2::formatAddress(record.object)
                      << " wrapper-fields=";
            for (const auto value : record.wrapperFields) {
                std::cout << saturn::sh2::formatAddress(value) << ',';
            }
            std::cout << " object-fields=";
            for (const auto value : record.fields) {
                std::cout << saturn::sh2::formatAddress(value) << ',';
            }
            std::cout << " stack=";
            for (const auto value : record.stack) {
                std::cout << saturn::sh2::formatAddress(value) << ',';
            }
            std::cout << " regs=";
            for (const auto value : record.regs) {
                std::cout << saturn::sh2::formatAddress(value) << ',';
            }
            std::cout << '\n';
        }

        std::cout << "\nGFS allocator branches: total="
                  << memoryProbe->gfsAllocatorRecordCount() << '\n';
        const auto& allocatorRecords = memoryProbe->gfsAllocatorRecords();
        const auto allocatorStart = memoryProbe->gfsAllocatorRecordCount() > 32U
            ? memoryProbe->gfsAllocatorRecordCount() - 32U : 0U;
        for (std::size_t logical = allocatorStart;
             logical < memoryProbe->gfsAllocatorRecordCount(); ++logical) {
            const auto& record = allocatorRecords[logical % allocatorRecords.size()];
            std::cout << "  block=" << saturn::sh2::formatAddress(record.block)
                      << " step=" << record.context.step << " regs=";
            for (const auto value : record.regs) {
                std::cout << saturn::sh2::formatAddress(value) << ',';
            }
            std::cout << " stream=";
            for (const auto value : record.stream) {
                std::cout << saturn::sh2::formatAddress(value) << ',';
            }
            std::cout << '\n';
        }

        std::cout << "\nGFS capacity checks: total="
                  << memoryProbe->gfsCapacityRecordCount() << '\n';
        const auto& capacityRecords = memoryProbe->gfsCapacityRecords();
        const auto capacityStart = memoryProbe->gfsCapacityRecordCount() > 32U
            ? memoryProbe->gfsCapacityRecordCount() - 32U : 0U;
        for (std::size_t logical = capacityStart;
             logical < memoryProbe->gfsCapacityRecordCount(); ++logical) {
            const auto& record = capacityRecords[logical % capacityRecords.size()];
            std::cout << "  block=" << saturn::sh2::formatAddress(record.block)
                      << " step=" << record.context.step << " regs=";
            for (const auto value : record.regs) {
                std::cout << saturn::sh2::formatAddress(value) << ',';
            }
            std::cout << " stack=";
            for (const auto value : record.stack) {
                std::cout << saturn::sh2::formatAddress(value) << ',';
            }
            std::cout << '\n';
        }

        std::cout << "\nGFS pending descriptor/ring samples:";
        for (const auto address : std::array<saturn::core::Address, 5>{
                 0x060803BCU, 0x0609D09CU, 0x0609D1CCU,
                 0x060996F4U, 0x06099CCCU}) {
            std::cout << " " << saturn::sh2::formatAddress(address) << "="
                      << saturn::sh2::formatAddress(memoryProbe->ram32(address));
        }
        std::cout << " ring-now(+84,+88,+92,+96)=";
        for (const auto offset : std::array<std::uint32_t, 4>{84U, 88U, 92U, 96U}) {
            std::cout << saturn::sh2::formatAddress(memoryProbe->ram32(
                0x0607C038U + offset)) << ',';
        }
        std::cout << '\n';

        std::cout << "\nSCU DMA descriptor snapshots at 0x06034A84: total="
                  << memoryProbe->dmaDescriptorRecords().size() << '\n';
        for (const auto& record : memoryProbe->dmaDescriptorRecords()) {
            std::cout << "  step=" << record.step
                      << " caller=" << saturn::sh2::formatAddress(record.caller)
                      << " descriptor=" << saturn::sh2::formatAddress(record.descriptor)
                      << " source=" << saturn::sh2::formatAddress(record.fields[0])
                      << " destination=" << saturn::sh2::formatAddress(record.fields[1])
                      << " count=0x" << std::hex << record.fields[2]
                      << " add_base=0x" << record.fields[3]
                      << " add_flags=0x" << record.fields[4]
                      << " mode=0x" << record.fields[8] << std::dec
                      << " samples=";
            for (std::size_t index = 0; index < record.sourceSamples.size(); ++index) {
                std::cout << '+' << std::hex << kDmaSourceSampleOffsets[index]
                          << ':' << record.sourceSamples[index] << std::dec << ',';
            }
            std::cout << '\n';
        }

        std::cout << "\nGFS source writes at 0x060B49CC: total="
                  << memoryProbe->gfsSourceWriteCount()
                  << " nonzero=" << memoryProbe->gfsSourceNonZeroWriteCount()
                  << " boundary_records="
                  << memoryProbe->gfsSourceBoundaryWrites().size() << '\n';
        for (const auto& record : memoryProbe->gfsSourceBoundaryWrites()) {
            std::cout << "  step=" << record.context.step
                      << " offset=0x" << std::hex
                      << (record.address - kGfsSourceBase) << std::dec
                      << " address=" << saturn::sh2::formatAddress(record.address)
                      << " width=" << static_cast<unsigned>(record.width)
                      << " value=" << saturn::sh2::formatAddress(record.value)
                      << " block=" << saturn::sh2::formatAddress(record.context.blockAddress)
                      << " caller=" << saturn::sh2::formatAddress(record.context.callerBlock)
                      << '\n';
        }
        std::cout << "\nGFS task-object writes at 0x0606E45C (+58..+7C): total="
                  << memoryProbe->gfsTaskConfigWrites().size() << '\n';
        for (const auto& record : memoryProbe->gfsTaskConfigWrites()) {
            std::cout << "  step=" << record.context.step
                      << " offset=0x" << std::hex
                      << (record.address - kGfsTaskObjectAddress) << std::dec
                      << " value=" << saturn::sh2::formatAddress(record.value)
                      << " block=" << saturn::sh2::formatAddress(record.context.blockAddress)
                      << " caller=" << saturn::sh2::formatAddress(record.context.callerBlock)
                      << '\n';
        }
        std::cout << "\nGFS decoder/decompressor snapshots: total="
                  << memoryProbe->gfsDecodeRecords().size() << '\n';
        for (const auto& record : memoryProbe->gfsDecodeRecords()) {
            std::cout << "  step=" << record.step
                      << " block=" << saturn::sh2::formatAddress(record.block)
                      << " caller=" << saturn::sh2::formatAddress(record.caller)
                      << " sample_pointer=" << saturn::sh2::formatAddress(record.samplePointer)
                      << " r4=" << saturn::sh2::formatAddress(record.registers[0])
                      << " r5=" << saturn::sh2::formatAddress(record.registers[1])
                      << " r6=" << saturn::sh2::formatAddress(record.registers[2])
                      << " r7=" << saturn::sh2::formatAddress(record.registers[3])
                      << " r8=" << saturn::sh2::formatAddress(record.registers[4])
                      << " r9=" << saturn::sh2::formatAddress(record.registers[5])
                      << " r10=" << saturn::sh2::formatAddress(record.registers[6])
                      << " r11=" << saturn::sh2::formatAddress(record.registers[7])
                      << " r12=" << saturn::sh2::formatAddress(record.registers[8])
                      << " r13=" << saturn::sh2::formatAddress(record.registers[9])
                      << " r14=" << saturn::sh2::formatAddress(record.registers[10])
                      << " sp=" << saturn::sh2::formatAddress(record.registers[11])
                      << " input_words=";
            for (const auto value : record.inputWords) {
                std::cout << saturn::sh2::formatAddress(value) << ',';
            }
            std::cout << " stack_words=";
            for (const auto value : record.stackWords) {
                std::cout << saturn::sh2::formatAddress(value) << ',';
            }
            std::cout << '\n';
        }
        std::cout << "  setup/boundary snapshots="
                  << memoryProbe->gfsDecodeBoundaryRecords().size() << '\n';
        for (const auto& record : memoryProbe->gfsDecodeBoundaryRecords()) {
            std::cout << "  setup step=" << record.step
                      << " block=" << saturn::sh2::formatAddress(record.block)
                      << " caller=" << saturn::sh2::formatAddress(record.caller)
                      << " r4=" << saturn::sh2::formatAddress(record.registers[0])
                      << " r6=" << saturn::sh2::formatAddress(record.registers[2])
                      << " r7=" << saturn::sh2::formatAddress(record.registers[3])
                      << " r8=" << saturn::sh2::formatAddress(record.registers[4])
                      << " r14=" << saturn::sh2::formatAddress(record.registers[10])
                      << " sp=" << saturn::sh2::formatAddress(record.registers[11])
                      << " stack0/4/8="
                      << saturn::sh2::formatAddress(record.stackWords[0]) << '/'
                      << saturn::sh2::formatAddress(record.stackWords[1]) << '/'
                      << saturn::sh2::formatAddress(record.stackWords[2])
                      << " object_words=";
            if (record.block == 0x06041D14U) {
                for (const auto value : record.taskObjectWords) {
                    std::cout << saturn::sh2::formatAddress(value) << ',';
                }
            }
            std::cout << '\n';
        }

        constexpr saturn::core::Address kTaskHandleAddress = 0x060519A4U;
        const auto taskAddress = memoryProbe->ram32(kTaskHandleAddress);
        const auto taskVtable = memoryProbe->ram32(taskAddress);
        const auto updateMethod = taskVtable == 0U ? 0U
            : memoryProbe->ram32(static_cast<saturn::core::Address>(taskVtable + 0x1B4U));
        std::cout << "\nAsynchronous task object:\n"
                  << "  handle[060519A4]=" << saturn::sh2::formatAddress(taskAddress)
                  << " vtable=" << saturn::sh2::formatAddress(taskVtable)
                  << " update[+1B4]=" << saturn::sh2::formatAddress(updateMethod)
                  << '\n';
        if (taskAddress != 0U) {
            for (saturn::core::Address offset = 0U; offset < 0x40U; offset += 4U) {
                std::cout << "  task[+" << std::hex << std::setw(2)
                          << std::setfill('0') << offset << "]="
                          << saturn::sh2::formatAddress(memoryProbe->ram32(
                                 static_cast<saturn::core::Address>(taskAddress + offset)))
                          << std::dec << '\n';
            }
        }
        const auto resolvedTaskAddress = memoryProbe->resolvedTaskAddress();
        std::cout << "  resolved-task="
                  << saturn::sh2::formatAddress(resolvedTaskAddress) << '\n';
        if (resolvedTaskAddress != 0U) {
            for (saturn::core::Address offset = 0U; offset < 0x20U; offset += 4U) {
                std::cout << "  resolved[+" << std::hex << std::setw(2)
                          << std::setfill('0') << offset << "]="
                          << saturn::sh2::formatAddress(memoryProbe->ram32(
                                 static_cast<saturn::core::Address>(resolvedTaskAddress + offset)))
                          << std::dec << '\n';
            }
            if (resolvedTaskAddress >= 0x38U) {
                const auto objectBase = static_cast<saturn::core::Address>(
                    resolvedTaskAddress - 0x38U);
                std::cout << "  resolved-object-base="
                          << saturn::sh2::formatAddress(objectBase) << '\n';
                for (saturn::core::Address offset = 0x180U;
                     offset <= 0x1B4U; offset += 4U) {
                    std::cout << "  object[+" << std::hex << std::setw(3)
                              << std::setfill('0') << offset << "]="
                              << saturn::sh2::formatAddress(memoryProbe->ram32(
                                     static_cast<saturn::core::Address>(objectBase + offset)))
                              << std::dec << '\n';
                }
            }
        }

        const auto& eventWrites = memoryProbe->eventRecords();
        const auto eventWriteCount = memoryProbe->eventRecordCount();
        const auto eventWriteStart = eventWriteCount > eventWrites.size()
            ? eventWriteCount % eventWrites.size() : 0U;
        std::cout << "\nRecent event-structure and title-wait-flag writes ("
                  << eventWrites.size() << '/' << eventWriteCount
                  << ", addresses 06051940/44/48/A4/CA, 0605E7F6 and 0606C355/498):\n";
        if (eventWrites.empty()) std::cout << "  none\n";
        for (std::size_t index = 0; index < eventWrites.size(); ++index) {
            const auto& record = eventWrites[
                (eventWriteStart + index) % eventWrites.size()];
            std::cout << "  write" << static_cast<unsigned>(record.width) * 8U
                      << " [" << saturn::sh2::formatAddress(record.address)
                      << "]=" << saturn::sh2::formatAddress(record.value);
            printContext(record.context);
            std::cout << '\n';
        }

        const auto& titleWaitFlagWrites = memoryProbe->titleWaitFlagWrites();
        const auto titleWaitFlagWriteCount = memoryProbe->titleWaitFlagWriteCount();
        const auto titleWaitFlagWriteStart =
            titleWaitFlagWriteCount > titleWaitFlagWrites.size()
                ? titleWaitFlagWriteCount % titleWaitFlagWrites.size() : 0U;
        std::cout << "\nWrites overlapping title wait flag 0605E7F6 ("
                  << titleWaitFlagWrites.size() << '/' << titleWaitFlagWriteCount
                  << "):\n";
        if (titleWaitFlagWrites.empty()) std::cout << "  none\n";
        for (std::size_t index = 0; index < titleWaitFlagWrites.size(); ++index) {
            const auto& record = titleWaitFlagWrites[
                (titleWaitFlagWriteStart + index) % titleWaitFlagWrites.size()];
            std::cout << "  write" << static_cast<unsigned>(record.width) * 8U
                      << " [" << saturn::sh2::formatAddress(record.address)
                      << "]=" << saturn::sh2::formatAddress(record.value);
            printContext(record.context);
            std::cout << '\n';
        }

        const auto& transitions = memoryProbe->blockTransitions();
        const auto transitionCount = memoryProbe->blockTransitionCount();
        const auto transitionStart = transitionCount > transitions.size()
            ? transitionCount % transitions.size() : 0U;
        std::cout << "\nRecent focused block transitions ("
                  << transitions.size() << '/' << transitionCount << "):\n";
        if (transitions.empty()) std::cout << "  none\n";
        for (std::size_t index = 0; index < transitions.size(); ++index) {
            const auto& record = transitions[
                (transitionStart + index) % transitions.size()];
            std::cout << "  " << saturn::sh2::formatAddress(record.from)
                      << " -> " << saturn::sh2::formatAddress(record.to)
                      << " event[060519CA]="
                      << saturn::sh2::formatAddress(record.eventValue);
            printContext(record.context);
            std::cout << '\n';
        }

        const auto& callbackTransitions = memoryProbe->callbackTransitions();
        const auto callbackTransitionCount = memoryProbe->callbackTransitionCount();
        const auto callbackTransitionStart = callbackTransitionCount > callbackTransitions.size()
            ? callbackTransitionCount % callbackTransitions.size() : 0U;
        std::cout << "\nRecent 06034D72 transitions ("
                  << callbackTransitions.size() << '/' << callbackTransitionCount << "):\n";
        if (callbackTransitions.empty()) std::cout << "  none\n";
        for (std::size_t index = 0; index < callbackTransitions.size(); ++index) {
            const auto& record = callbackTransitions[
                (callbackTransitionStart + index) % callbackTransitions.size()];
            std::cout << "  " << saturn::sh2::formatAddress(record.from)
                      << " -> " << saturn::sh2::formatAddress(record.to)
                      << " event[060519CA]="
                      << saturn::sh2::formatAddress(record.eventValue);
            printContext(record.context);
            std::cout << '\n';
        }

        const auto& asyncTransitions = memoryProbe->asyncTransitions();
        const auto asyncTransitionCount = memoryProbe->asyncTransitionCount();
        const auto asyncTransitionStart = asyncTransitionCount > asyncTransitions.size()
            ? asyncTransitionCount % asyncTransitions.size() : 0U;
        std::cout << "\nRecent asynchronous task transitions ("
                  << asyncTransitions.size() << '/' << asyncTransitionCount << "):\n";
        if (asyncTransitions.empty()) std::cout << "  none\n";
        for (std::size_t index = 0; index < asyncTransitions.size(); ++index) {
            const auto& record = asyncTransitions[
                (asyncTransitionStart + index) % asyncTransitions.size()];
            std::cout << "  " << saturn::sh2::formatAddress(record.from)
                      << " -> " << saturn::sh2::formatAddress(record.to)
                      << " event[060519CA]="
                      << saturn::sh2::formatAddress(record.eventValue);
            printContext(record.context);
            std::cout << '\n';
        }

        const auto& waitEvents = memoryProbe->waitEventRecords();
        const auto waitEventCount = memoryProbe->waitEventRecordCount();
        const auto waitEventStart = waitEventCount > waitEvents.size()
            ? waitEventCount % waitEvents.size() : 0U;
        std::cout << "\nRecent 060519CA accesses ("
                  << waitEvents.size() << '/' << waitEventCount
                  << ", reads=" << memoryProbe->waitEventReadCount()
                  << ", writes=" << memoryProbe->waitEventWriteCount() << "):\n";
        if (waitEvents.empty()) std::cout << "  none\n";
        for (std::size_t index = 0; index < waitEvents.size(); ++index) {
            const auto& record = waitEvents[
                (waitEventStart + index) % waitEvents.size()];
            std::cout << "  " << (record.write ? "write" : "read")
                      << static_cast<unsigned>(record.width) * 8U
                      << " [" << saturn::sh2::formatAddress(record.address)
                      << "]=" << saturn::sh2::formatAddress(record.value);
            printContext(record.context);
            std::cout << '\n';
        }

        std::cout << "\n060519CA writes:\n";
        if (memoryProbe->waitEventWrites().empty()) std::cout << "  none\n";
        for (const auto& record : memoryProbe->waitEventWrites()) {
            std::cout << "  write" << static_cast<unsigned>(record.width) * 8U
                      << " [" << saturn::sh2::formatAddress(record.address)
                      << "]=" << saturn::sh2::formatAddress(record.value);
            printContext(record.context);
            std::cout << '\n';
        }
    }
    if (arguments.traceForeground) {
        std::cout << "\nRecent foreground trace (dispatcher omitted):\n";
        const auto shown = std::min(recentForegroundCount, recentForeground.size());
        const auto start = recentForegroundCount > recentForeground.size()
            ? recentForegroundCount % recentForeground.size() : 0U;
        for (std::size_t index = 0; index < shown; ++index) {
            const auto& [address, step] =
                recentForeground[(start + index) % recentForeground.size()];
            std::cout << "  step " << step << ' '
                      << saturn::sh2::formatAddress(address) << '\n';
        }
    }
    if (reachedExpectedEntry ||
        execution.status == saturn::sir::ProgramStatus::FrontendQuit) {
        return controlTraceWriteFailure ? 1 : 0;
    }
    if (controlTraceWriteFailure) return 1;
    return execution.status == saturn::sir::ProgramStatus::InterpreterFailure ? 1 : 2;
}

} // namespace

int main(int argc, char** argv) {
    const auto arguments = parseArguments(argc, argv);
    if (!arguments ||
        (arguments->command != "decode" && arguments->command != "cfg" &&
         arguments->command != "analyze" && arguments->command != "run")) {
        if (arguments && (arguments->command == "boot" ||
                          arguments->command == "launch")) {
            const auto hasBootBase = arguments->ipImage
                ? arguments->hasIpBase
                : arguments->hasAipBase;
            if (!hasBootBase || !arguments->program ||
                !arguments->hasProgramBase) {
                usage(std::cerr);
                return 2;
            }
            return boot(*arguments);
        }
        usage(std::cerr);
        return 2;
    }
    if (!arguments->hasBase) {
        usage(std::cerr);
        return 2;
    }
    if ((arguments->command == "cfg" || arguments->command == "analyze" ||
         arguments->command == "run") &&
        !arguments->entry) {
        std::cerr << "error: " << arguments->command << " requires --entry\n";
        usage(std::cerr);
        return 2;
    }

    const auto image = saturn::core::MemoryMap::fromFile(arguments->input, arguments->base);
    if (!image) {
        std::cerr << "error: unable to load " << arguments->input.string() << '\n';
        return 1;
    }

    if (arguments->command == "decode") {
        return decode(*image);
    }
    if (arguments->command == "analyze") {
        return analyze(*image, *arguments->entry);
    }
    if (arguments->command == "run") {
        return run(*image, *arguments->entry, arguments->maxSteps, arguments->forceInterpreter);
    }
    return cfg(*image, *arguments->entry, arguments->output);
}
