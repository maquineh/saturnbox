#include "../tools/saturn-sh2/src/live_debug.hpp"
#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

struct TempTree {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("saturn-live-debug-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~TempTree() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

saturn::platform::VideoDebugSnapshot snapshot(std::uint32_t pixel, bool memory = false) {
    saturn::platform::VideoDebugSnapshot s;
    s.composed.width = 1;
    s.composed.height = 1;
    s.composed.pixels = {pixel};
    s.framebufferBanks[0] = {1, 2, 3};
    s.framebufferBanks[1] = {4, 5, 6};
    s.frameCount = pixel;
    s.compositionCount = pixel + 10;
    s.contextBlock = 0x06010000;
    s.vdp1Fbcr = 3;
    if (memory) {
        s.vdp1Vram = {0xaa, 0xbb};
        s.vdp2Vram = {0xcc};
        s.cram = {0xdd};
        s.vdp1Registers = {0x10, 0x20};
    }
    return s;
}

saturn::tools::DebugMetadata metadata(std::uint64_t step) {
    saturn::tools::DebugMetadata m;
    m.utcEpochMs = 123456 + step;
    m.elapsedMs = step * 16;
    m.step = step;
    m.block = 0x06010000;
    m.caller = 0x06020000;
    m.playerXRaw = 0x12340000;
    m.playerX = 0x1234;
    m.pad = 0xfffe;
    m.scspStats = "irq=2,slots=1";
    m.audioUnderruns = 7;
    return m;
}

std::size_t countLines(const std::filesystem::path& path) {
    std::ifstream f(path);
    std::size_t count = 0;
    std::string line;
    while (std::getline(f, line)) ++count;
    return count;
}

void testLiveDebugPrePostAndFiles() {
    TempTree temp;
    saturn::tools::LiveDebugSession session(temp.path / "run");
    for (std::uint64_t i = 0; i < 10; ++i) session.recordFrame(snapshot(static_cast<std::uint32_t>(i)), metadata(i));
    {
        std::ofstream request(session.directory() / "capture.request", std::ios::binary);
        request << "inspect boss entrance\n";
    }
    const auto reason = session.pollRequest();
    REQUIRE(reason.has_value());
    REQUIRE(*reason == "inspect boss entrance");
    REQUIRE(session.isCapturing());
    auto attached = metadata(10);
    attached.attachments.push_back({"keyframe.ram", {9, 8, 7}});
    session.recordPhase("before-render", snapshot(20, true), metadata(10));
    for (std::uint64_t i = 0; i < 8; ++i) session.recordFrame(snapshot(static_cast<std::uint32_t>(20 + i), true),
                                                              i == 0 ? std::move(attached) : metadata(11 + i));
    REQUIRE(!session.isCapturing());
    session.flush();
    const auto capture = session.directory() / "capture0001";
    REQUIRE(std::filesystem::exists(capture / "pre_0000.ppm"));
    REQUIRE(std::filesystem::exists(capture / "pre_0007_vdp1_bank1.raw"));
    REQUIRE(std::filesystem::exists(capture / "phase_0008_before-render_vdp1_vram.bin"));
    REQUIRE(std::filesystem::exists(capture / "post_0009_keyframe.ram"));
    REQUIRE(std::filesystem::exists(capture / "telemetry.csv"));
    REQUIRE(countLines(capture / "telemetry.csv") == 18); // header + 8 pre + phase + 8 post
    REQUIRE(std::filesystem::exists(session.directory() / "status.txt"));
    REQUIRE(!session.pollRequest().has_value());
}

void testLiveDebugRequestApiBoundsAndUniqueSession() {
    TempTree temp;
    std::filesystem::create_directories(temp.path / "same");
    { std::ofstream old(temp.path / "same" / "keep.txt"); old << "keep"; }
    saturn::tools::LiveDebugSession first(temp.path / "same");
    saturn::tools::LiveDebugSession second(temp.path / "same");
    REQUIRE(first.directory() != second.directory());
    REQUIRE(std::filesystem::exists(temp.path / "same" / "keep.txt"));
    REQUIRE(first.request(std::string(700, 'x')));
    std::ifstream f(first.directory() / "capture.request", std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), {});
    REQUIRE(text.size() <= 514); // 512 reason bytes and newline
    REQUIRE(first.pollRequest().has_value());
    REQUIRE(first.isCapturing());
}

void testLiveDebugInvalidPathReportsError() {
    TempTree temp;
    std::filesystem::create_directories(temp.path);
    const auto blocker = temp.path / "not-directory";
    { std::ofstream f(blocker); f << 'x'; }
    saturn::tools::LiveDebugSession session(blocker / "session");
    REQUIRE(session.hasError());
    REQUIRE(!session.lastError().empty());
}

void testLiveDebugReservesPostFramesAndKeepsPendingRequest() {
    TempTree temp;
    saturn::tools::LiveDebugSession session(temp.path / "limits");
    for (unsigned i = 0; i < 8; ++i) session.recordFrame(snapshot(i), metadata(i));
    REQUIRE(session.request("first"));
    REQUIRE(!session.request("must not overwrite pending first"));
    REQUIRE(session.pollRequest() == "first");
    REQUIRE(session.request("second"));
    REQUIRE(!session.pollRequest().has_value()); // Busy requests remain queued.
    for (unsigned i = 0; i < 80; ++i) session.recordPhase("after-draw", snapshot(i), metadata(i));
    for (unsigned i = 0; i < 8; ++i) session.recordFrame(snapshot(i), metadata(i));
    REQUIRE(!session.isCapturing());
    REQUIRE(session.pollRequest() == "second");
    for (unsigned i = 0; i < 8; ++i) session.recordFrame(snapshot(i), metadata(i));
    session.flush();
    std::ifstream rows(session.directory() / "capture0001" / "telemetry.csv");
    std::string row;
    unsigned posts{};
    while (std::getline(rows, row)) if (row.find(",post,") != std::string::npos) ++posts;
    REQUIRE(posts == 8U);
    REQUIRE(session.droppedSnapshots() > 0U);
    REQUIRE(std::filesystem::exists(session.directory() / "capture0002" / "telemetry.csv"));
}

} // namespace

void registerLiveDebugTests(test::Registry& registry) {
    registry.add("live debug captures pre/post snapshots and files", testLiveDebugPrePostAndFiles);
    registry.add("live debug request bounds and unique session", testLiveDebugRequestApiBoundsAndUniqueSession);
    registry.add("live debug reports invalid output path", testLiveDebugInvalidPathReportsError);
    registry.add("live debug reserves post frames and retains busy requests", testLiveDebugReservesPostFramesAndKeepsPendingRequest);
}
