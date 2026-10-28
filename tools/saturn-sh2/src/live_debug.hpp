#pragma once

#include "saturn/saturn/video.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace saturn::tools {

struct DebugAttachment {
    std::string name;
    std::vector<std::uint8_t> bytes;
};

struct DebugMetadata {
    std::uint64_t utcEpochMs{};
    std::uint64_t elapsedMs{};
    std::uint64_t step{};
    core::Address block{};
    core::Address caller{};
    std::uint32_t playerXRaw{};
    std::uint32_t playerX{};
    std::uint16_t pad{};
    std::string scspStats;
    std::uint64_t audioUnderruns{};
    std::vector<DebugAttachment> attachments;
};

// Lightweight game-thread recorder. Capture serialization is performed by a
// bounded background worker; capture.request is inert UTF-8 text, never code.
class LiveDebugSession {
public:
    explicit LiveDebugSession(std::filesystem::path directory);
    ~LiveDebugSession();
    LiveDebugSession(const LiveDebugSession&) = delete;
    LiveDebugSession& operator=(const LiveDebugSession&) = delete;

    // Writes a request for pollRequest() to consume. The reason is clipped to
    // 512 bytes. External tools may write the same fixed capture.request file.
    bool request(std::string_view reason);
    std::optional<std::string> pollRequest();

    // recordFrame maintains the eight-frame pre-ring and the eight-frame post
    // window. recordPhase emits an extra phase snapshot only during capture.
    void recordFrame(platform::VideoDebugSnapshot snapshot, DebugMetadata metadata);
    void recordPhase(std::string_view phase, platform::VideoDebugSnapshot snapshot,
                     DebugMetadata metadata);

    bool isCapturing() const noexcept;
    std::uint64_t droppedSnapshots() const noexcept;
    bool hasError() const noexcept;
    std::string lastError() const;
    const std::filesystem::path& directory() const noexcept;
    void flush();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace saturn::tools
