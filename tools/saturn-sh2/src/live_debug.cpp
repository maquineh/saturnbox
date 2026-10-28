#include "live_debug.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace saturn::tools {
namespace {

constexpr std::size_t kPreFrames = 8;
constexpr std::size_t kPostFrames = 8;
constexpr std::size_t kMaxRecordsPerCapture = 48;
constexpr std::size_t kQueueLimit = 64;
constexpr std::size_t kMaxCapturesPerSession = 64;
constexpr std::uint64_t kMaxSessionBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxQueuedBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxPreRingBytes = 128ULL * 1024ULL * 1024ULL;

std::string csv(std::string_view value) {
    std::string result{"\""};
    for (const char c : value) {
        if (c == '"') result += "\"\"";
        else if (c == '\r' || c == '\n') result += ' ';
        else result += c;
    }
    result += '"';
    return result;
}

std::string safeName(std::string_view value) {
    std::string out;
    out.reserve(std::min<std::size_t>(value.size(), 80));
    for (const unsigned char c : value) {
        if (out.size() >= 80) break;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') out += static_cast<char>(c);
        else out += '_';
    }
    if (out.empty() || out == "." || out == "..") out = "attachment.bin";
    return out;
}

std::uint64_t estimateBytes(const platform::VideoDebugSnapshot& s, const DebugMetadata& m) {
    std::uint64_t total = s.composed.pixels.size() * sizeof(std::uint32_t);
    for (const auto& bank : s.framebufferBanks) total += bank.size();
    total += s.vdp1Vram.size() + s.vdp2Vram.size() + s.cram.size();
    total += s.vdp1Registers.size() + s.vdp2Registers.size();
    for (const auto& a : m.attachments) total += a.bytes.size();
    total += (s.vdp1RegisterWriteHistory.size() + s.vdp1MemoryWriteHistory.size()) * 64ULL;
    for (const auto& c : s.vdp1CommandHistory) total += c.commands.size() * sizeof(platform::Vdp1CommandTraceEntry);
    return total;
}

bool writeBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) return true;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

bool writePpm(const std::filesystem::path& path, const platform::VideoFrame& frame) {
    if (frame.width == 0 || frame.height == 0 ||
        static_cast<std::uint64_t>(frame.width) * frame.height > frame.pixels.size()) return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << "P6\n" << frame.width << ' ' << frame.height << "\n255\n";
    for (const auto pixel : frame.pixels) {
        const std::array<char, 3> rgb{static_cast<char>((pixel >> 16U) & 0xffU),
                                      static_cast<char>((pixel >> 8U) & 0xffU),
                                      static_cast<char>(pixel & 0xffU)};
        f.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
    }
    return static_cast<bool>(f);
}

} // namespace

struct LiveDebugSession::Impl {
    struct Record {
        std::string kind;
        std::string phase;
        platform::VideoDebugSnapshot snapshot;
        DebugMetadata metadata;
        std::uint64_t capture{};
        std::size_t sequence{};
    };

    explicit Impl(std::filesystem::path requested) {
        std::error_code ec;
        std::filesystem::create_directories(requested, ec);
        if (ec) {
            base = std::move(requested);
            setError("cannot create requested directory: " + ec.message());
        } else {
            // Keep artifacts from prior runs intact by giving every session a
            // unique child directory, including when the parent is empty.
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            for (unsigned suffix = 0; suffix < 1000; ++suffix) {
                auto candidate = requested / ("session-" + std::to_string(now) + "-" + std::to_string(suffix));
                ec.clear();
                if (std::filesystem::create_directory(candidate, ec)) { base = std::move(candidate); break; }
                if (ec && ec != std::errc::file_exists) break;
            }
            if (base.empty()) {
                base = std::move(requested);
                setError("cannot allocate unique session directory");
            }
        }
        if (errorText.empty()) {
            std::ofstream meta(base / "session.txt", std::ios::binary | std::ios::trunc);
            if (!meta) setError("cannot write session.txt");
            else meta << "format=saturn-live-debug-v1\npre_frames=8\npost_frames=8\nqueue_limit=64\n";
        }
        writeStatus();
        worker = std::thread([this] { workerLoop(); });
    }

    ~Impl() {
        flush();
        { std::lock_guard lock(queueMutex); stopping = true; }
        queueCv.notify_all();
        if (worker.joinable()) worker.join();
        writeStatus();
    }

    void setError(std::string text) {
        std::lock_guard lock(statusMutex);
        if (errorText.empty()) errorText = std::move(text);
    }

    void writeStatus() {
        if (base.empty()) return;
        std::lock_guard fileLock(statusFileMutex);
        std::string error;
        { std::lock_guard lock(statusMutex); error = errorText; }
        std::ofstream f(base / "status.txt", std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << "capturing=" << (capturing.load() ? 1 : 0) << "\n"
          << "dropped_snapshots=" << dropped.load() << "\n"
          << "captures_started=" << captureCount.load() << "\n"
          << "capture_limit=" << kMaxCapturesPerSession << "\n"
          << "session_byte_limit=" << kMaxSessionBytes << "\n"
          << "queue_byte_limit=" << kMaxQueuedBytes << "\n"
          << "bytes_written_estimate=" << bytesEstimate.load() << "\n"
          << "bytes_reserved=" << reservedBytes.load() << "\n"
          << "error=" << error << "\n";
    }

    std::string lastError() const {
        std::lock_guard lock(statusMutex);
        return errorText;
    }

    void drop(std::string_view why) {
        dropped.fetch_add(1, std::memory_order_relaxed);
        if (!why.empty()) setError(std::string(why));
    }

    bool enqueue(Record&& record) {
        const auto size = estimateBytes(record.snapshot, record.metadata);
        std::unique_lock lock(queueMutex, std::try_to_lock);
        if (!lock.owns_lock()) { drop("queue busy; snapshots dropped"); return false; }
        if (stopping || queue.size() >= kQueueLimit || size > kMaxQueuedBytes - std::min(queuedBytes, kMaxQueuedBytes) ||
            size > kMaxSessionBytes - std::min(reservedBytes.load(), kMaxSessionBytes)) {
            lock.unlock(); drop("queue full/byte limit/stopping; snapshots dropped"); return false;
        }
        queue.push_back(std::move(record));
        queuedBytes += size;
        reservedBytes += size;
        lock.unlock();
        queueCv.notify_one();
        return true;
    }

    bool beginCapture(std::string reason) {
        if (captureCount.load() >= kMaxCapturesPerSession || reservedBytes.load() >= kMaxSessionBytes) {
            drop("session capture/size limit reached");
            return false;
        }
        std::lock_guard stateLock(stateMutex);
        if (capturing.load()) return false;
        const auto id = captureCount.fetch_add(1) + 1;
        currentCapture = id;
        recordSequence = 0;
        captureRecords = 0;
        attachmentWritten = false;
        remainingPost = kPostFrames;
        capturing.store(true);
        const auto processedPath = base / ("capture" + captureName(id) + ".request.txt");
        std::error_code renameError;
        std::filesystem::rename(base / "capture.request", processedPath, renameError);
        if (renameError) {
            std::ofstream info(processedPath, std::ios::binary | std::ios::trunc);
            if (info) info << reason << '\n';
            setError("could not move capture.request after accepting it: " + renameError.message());
        }
        for (auto& rec : preRing) {
            rec.kind = "pre";
            rec.capture = id;
            rec.sequence = recordSequence++;
            ++captureRecords;
            enqueue(std::move(rec));
        }
        preRing.clear();
        preRingBytes = 0;
        writeStatus();
        return true;
    }

    static std::string captureName(std::uint64_t id) {
        std::ostringstream s; s << std::setw(4) << std::setfill('0') << id; return s.str();
    }

    void finishCapture() {
        capturing.store(false);
        writeStatus();
    }

    void appendTelemetry(const std::filesystem::path& dir, const Record& r) {
        const auto path = dir / "telemetry.csv";
        const bool fresh = !std::filesystem::exists(path);
        std::ofstream f(path, std::ios::binary | std::ios::app);
        if (!f) { setError("cannot append telemetry.csv"); return; }
        if (fresh) f << "sequence,kind,phase,utc_epoch_ms,elapsed_ms,step,block,caller,player_x_raw,player_x,pad,scsp_stats,audio_underruns,video_frame,composition,composition_dirty,frame_pending\n";
        const auto& s = r.snapshot;
        f << r.sequence << ',' << r.kind << ',' << csv(r.phase) << ',' << r.metadata.utcEpochMs << ','
          << r.metadata.elapsedMs << ',' << r.metadata.step << ',' << r.metadata.block << ',' << r.metadata.caller << ','
          << r.metadata.playerXRaw << ',' << r.metadata.playerX << ',' << r.metadata.pad << ','
          << csv(r.metadata.scspStats) << ',' << r.metadata.audioUnderruns << ',' << s.frameCount << ','
          << s.compositionCount << ',' << (s.compositionDirty ? 1 : 0) << ',' << (s.framePending ? 1 : 0) << '\n';
    }

    void appendRootTelemetry(const Record& r) {
        const auto path = base / "telemetry.csv";
        const bool fresh = !std::filesystem::exists(path);
        std::ofstream f(path, std::ios::binary | std::ios::app);
        if (!f) { setError("cannot append session telemetry.csv"); return; }
        if (fresh) f << "utc_epoch_ms,elapsed_ms,step,block,caller,player_x_raw,player_x,pad,scsp_stats,audio_underruns,video_frame,composition,display_bank,draw_bank,swap_pending,composition_dirty,frame_pending\n";
        const auto& s = r.snapshot;
        f << r.metadata.utcEpochMs << ',' << r.metadata.elapsedMs << ',' << r.metadata.step << ','
          << r.metadata.block << ',' << r.metadata.caller << ',' << r.metadata.playerXRaw << ','
          << r.metadata.playerX << ',' << r.metadata.pad << ',' << csv(r.metadata.scspStats) << ','
          << r.metadata.audioUnderruns << ',' << s.frameCount << ',' << s.compositionCount << ','
          << unsigned(s.vdp1DisplayBank) << ',' << unsigned(s.vdp1DrawBank) << ','
          << (s.vdp1SwapPending ? 1 : 0) << ',' << (s.compositionDirty ? 1 : 0) << ','
          << (s.framePending ? 1 : 0) << '\n';
    }

    void writeRecord(Record& r) {
        if (r.kind == "telemetry") { appendRootTelemetry(r); return; }
        const auto dir = base / ("capture" + captureName(r.capture));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) { setError("cannot create capture directory: " + ec.message()); return; }
        auto stem = r.kind + "_" + captureName(r.sequence);
        if (r.kind == "phase") stem += "_" + safeName(r.phase);
        bool ok = writePpm(dir / (stem + ".ppm"), r.snapshot.composed);
        ok = writeBytes(dir / (stem + "_vdp1_bank0.raw"), r.snapshot.framebufferBanks[0]) && ok;
        ok = writeBytes(dir / (stem + "_vdp1_bank1.raw"), r.snapshot.framebufferBanks[1]) && ok;
        ok = writeBytes(dir / (stem + "_vdp1_vram.bin"), r.snapshot.vdp1Vram) && ok;
        ok = writeBytes(dir / (stem + "_vdp2_vram.bin"), r.snapshot.vdp2Vram) && ok;
        ok = writeBytes(dir / (stem + "_cram.bin"), r.snapshot.cram) && ok;
        ok = writeBytes(dir / (stem + "_vdp1_registers.bin"), r.snapshot.vdp1Registers) && ok;
        ok = writeBytes(dir / (stem + "_vdp2_registers.bin"), r.snapshot.vdp2Registers) && ok;
        {
            std::ofstream f(dir / (stem + "_video.txt"), std::ios::binary | std::ios::trunc);
            if (!f) ok = false;
            else {
                const auto& s = r.snapshot;
                f << "frame=" << s.frameCount << "\ncomposition=" << s.compositionCount
                  << "\ncontext_step=" << s.contextStep << "\ncontext_block=" << s.contextBlock
                  << "\ncontext_caller=" << s.contextCaller << "\nTVMR=" << s.vdp1Tvmr
                  << "\nFBCR=" << s.vdp1Fbcr << "\nPTMR=" << s.vdp1Ptmr << "\nEWDR=" << s.vdp1Ewdr
                  << "\nEWLR=" << s.vdp1Ewlr << "\nEWRR=" << s.vdp1Ewrr << "\nEDSR=" << s.vdp1Edsr
                  << "\nLOPR=" << s.vdp1Lopr << "\nCOPR=" << s.vdp1Copr
                  << "\ndisplay_bank=" << unsigned(s.vdp1DisplayBank) << "\ndraw_bank=" << unsigned(s.vdp1DrawBank)
                  << "\nswap_pending=" << s.vdp1SwapPending << "\nerase_pending=" << s.vdp1ErasePending
                  << "\nvblank_erase_pending=" << s.vdp1VblankErasePending << "\nin_vblank=" << s.vdp1InVBlank
                  << "\ndraw_end_pending=" << s.vdp1DrawEndPending << "\nframebuffer_configured=" << s.vdp1FramebufferConfigured
                  << "\ncomposition_dirty=" << s.compositionDirty << "\nframe_pending=" << s.framePending << '\n';
            }
        }
        {
            std::ofstream f(dir / (stem + "_traces.txt"), std::ios::binary | std::ios::trunc);
            if (!f) ok = false;
            else {
                const auto& s = r.snapshot;
                f << "VDP1 register write history: " << s.vdp1RegisterWriteHistory.size() << '\n';
                for (const auto& e : s.vdp1RegisterWriteHistory)
                    f << e.step << ',' << e.blockAddress << ',' << e.callerBlock << ',' << e.offset << ',' << unsigned(e.width) << ',' << e.value << '\n';
                f << "VDP1 memory write history: " << s.vdp1MemoryWriteHistory.size() << '\n';
                for (const auto& e : s.vdp1MemoryWriteHistory)
                    f << e.step << ',' << e.blockAddress << ',' << e.callerBlock << ',' << e.offset << ',' << unsigned(e.width) << ',' << e.value << '\n';
                f << "VDP1 command history: " << s.vdp1CommandHistory.size() << '\n';
                for (const auto& execution : s.vdp1CommandHistory) {
                    f << "execution," << execution.drawFrame << ',' << execution.presentedFrame << ',' << execution.wasPresented
                      << ',' << unsigned(execution.drawBank) << ',' << unsigned(execution.displayBankAtDraw) << ','
                      << unsigned(execution.displayedBank) << ',' << execution.stats.commands << ',' << execution.stats.sprites
                      << ',' << execution.stats.polygons << ',' << execution.stats.lines << ',' << execution.stats.skipped
                      << ',' << execution.stats.malformed << ',' << execution.stats.lastAddress << '\n';
                    for (const auto& command : execution.commands) {
                        f << "command," << command.address;
                        for (const auto word : command.words) f << ',' << word;
                        f << '\n';
                    }
                }
            }
        }
        for (const auto& attachment : r.metadata.attachments) {
            if (!writeBytes(dir / (stem + "_" + safeName(attachment.name)), attachment.bytes)) ok = false;
        }
        appendTelemetry(dir, r);
        const auto size = estimateBytes(r.snapshot, r.metadata);
        const auto written = bytesEstimate.fetch_add(size) + size;
        if (written > kMaxSessionBytes) setError("session output byte limit exceeded");
        if (!ok) setError("one or more capture files could not be written");
        writeStatus();
    }

    void workerLoop() {
        for (;;) {
            Record record;
            {
                std::unique_lock lock(queueMutex);
                queueCv.wait(lock, [this] { return stopping || !queue.empty(); });
                if (stopping && queue.empty()) break;
                record = std::move(queue.front());
                queue.pop_front();
                queuedBytes -= std::min(queuedBytes, estimateBytes(record.snapshot, record.metadata));
                writing = true;
            }
            try { writeRecord(record); }
            catch (const std::exception& error) { setError(std::string("capture worker: ") + error.what()); }
            { std::lock_guard lock(queueMutex); writing = false; if (queue.empty()) drainedCv.notify_all(); }
        }
    }

    void flush() {
        std::unique_lock lock(queueMutex);
        drainedCv.wait(lock, [this] { return queue.empty() && !writing; });
    }

    std::filesystem::path base;
    std::deque<Record> preRing;
    mutable std::mutex stateMutex;
    std::mutex queueMutex;
    std::condition_variable queueCv, drainedCv;
    std::deque<Record> queue;
    std::uint64_t queuedBytes{}, preRingBytes{};
    std::atomic<std::uint64_t> reservedBytes{};
    std::thread worker;
    bool stopping{}, writing{};
    bool attachmentWritten{};
    std::uint64_t lastTelemetryEpochMs{};
    std::chrono::steady_clock::time_point lastTelemetrySteady{};
    std::size_t remainingPost{}, captureRecords{}, recordSequence{};
    std::uint64_t currentCapture{};
    std::atomic<bool> capturing{};
    std::atomic<std::uint64_t> dropped{}, captureCount{}, bytesEstimate{};
    mutable std::mutex statusMutex;
    std::mutex statusFileMutex;
    std::string errorText;
    std::filesystem::file_time_type lastRequestTime{};
    std::uintmax_t lastRequestSize{std::numeric_limits<std::uintmax_t>::max()};
    std::optional<std::string> pendingReason;
};

LiveDebugSession::LiveDebugSession(std::filesystem::path directory)
    : impl_(std::make_unique<Impl>(std::move(directory))) {}
LiveDebugSession::~LiveDebugSession() = default;

bool LiveDebugSession::request(std::string_view reason) {
    std::error_code ec;
    if (std::filesystem::exists(impl_->base / "capture.request", ec)) {
        impl_->setError("capture.request already pending; refusing to overwrite it");
        impl_->writeStatus();
        return false;
    }
    const auto n = std::min<std::size_t>(reason.size(), 512);
    std::ofstream f(impl_->base / "capture.request", std::ios::binary | std::ios::trunc);
    if (!f) { impl_->setError("cannot write capture.request"); impl_->writeStatus(); return false; }
    f.write(reason.data(), static_cast<std::streamsize>(n));
    f.put('\n');
    return static_cast<bool>(f);
}

std::optional<std::string> LiveDebugSession::pollRequest() {
    if (impl_->capturing.load()) return std::nullopt;
    const auto path = impl_->base / "capture.request";
    if (!impl_->pendingReason) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) return std::nullopt;
        const auto stamp = std::filesystem::last_write_time(path, ec);
        if (ec) return std::nullopt;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || (stamp == impl_->lastRequestTime && size == impl_->lastRequestSize)) return std::nullopt;
        std::ifstream f(path, std::ios::binary);
        if (!f) { impl_->setError("cannot read capture.request"); impl_->writeStatus(); return std::nullopt; }
        std::string reason(512, '\0');
        f.read(reason.data(), static_cast<std::streamsize>(reason.size()));
        reason.resize(static_cast<std::size_t>(f.gcount()));
        while (!reason.empty() && (reason.back() == '\r' || reason.back() == '\n')) reason.pop_back();
        impl_->pendingReason = std::move(reason);
    }
    if (impl_->beginCapture(*impl_->pendingReason)) {
        auto accepted = std::move(impl_->pendingReason);
        impl_->pendingReason.reset();
        return accepted;
    }
    return std::nullopt;
}

void LiveDebugSession::recordFrame(platform::VideoDebugSnapshot snapshot, DebugMetadata metadata) {
    std::unique_lock state(impl_->stateMutex, std::try_to_lock);
    if (!state.owns_lock()) { impl_->drop("state busy; frame dropped"); return; }
    const auto steadyNow = std::chrono::steady_clock::now();
    const bool telemetryDue = impl_->lastTelemetrySteady.time_since_epoch().count() == 0 ||
        steadyNow - impl_->lastTelemetrySteady >= std::chrono::seconds(1);
    if (telemetryDue) {
        Impl::Record telemetry;
        telemetry.kind = "telemetry";
        telemetry.metadata = metadata;
        telemetry.snapshot.frameCount = snapshot.frameCount;
        telemetry.snapshot.compositionCount = snapshot.compositionCount;
        telemetry.snapshot.vdp1DisplayBank = snapshot.vdp1DisplayBank;
        telemetry.snapshot.vdp1DrawBank = snapshot.vdp1DrawBank;
        telemetry.snapshot.vdp1SwapPending = snapshot.vdp1SwapPending;
        telemetry.snapshot.compositionDirty = snapshot.compositionDirty;
        telemetry.snapshot.framePending = snapshot.framePending;
        impl_->enqueue(std::move(telemetry));
        impl_->lastTelemetrySteady = steadyNow;
    }
    if (!impl_->capturing.load()) {
        Impl::Record rec;
        rec.kind = "pre";
        rec.snapshot = std::move(snapshot);
        rec.metadata = std::move(metadata);
        const auto bytes = estimateBytes(rec.snapshot, rec.metadata);
        if (bytes > kMaxPreRingBytes) { impl_->drop("pre-ring snapshot exceeded byte limit"); return; }
        while (!impl_->preRing.empty() &&
               (impl_->preRing.size() == kPreFrames || bytes > kMaxPreRingBytes - impl_->preRingBytes)) {
            impl_->preRingBytes -= std::min(impl_->preRingBytes,
                estimateBytes(impl_->preRing.front().snapshot, impl_->preRing.front().metadata));
            impl_->preRing.pop_front();
        }
        impl_->preRingBytes += bytes;
        impl_->preRing.push_back(std::move(rec));
        return;
    }
    if (impl_->captureRecords >= kMaxRecordsPerCapture || impl_->reservedBytes.load() >= kMaxSessionBytes) {
        impl_->drop("capture record/size limit reached");
        if (impl_->remainingPost > 0 && --impl_->remainingPost == 0) impl_->finishCapture();
        return;
    }
    Impl::Record rec;
    rec.kind = "post"; rec.snapshot = std::move(snapshot); rec.metadata = std::move(metadata);
    if (impl_->attachmentWritten) rec.metadata.attachments.clear();
    else if (!rec.metadata.attachments.empty()) impl_->attachmentWritten = true;
    rec.capture = impl_->currentCapture; rec.sequence = impl_->recordSequence++;
    ++impl_->captureRecords;
    impl_->enqueue(std::move(rec));
    if (impl_->remainingPost > 0 && --impl_->remainingPost == 0) impl_->finishCapture();
}

void LiveDebugSession::recordPhase(std::string_view phase, platform::VideoDebugSnapshot snapshot,
                                   DebugMetadata metadata) {
    if (!impl_->capturing.load()) return;
    std::unique_lock state(impl_->stateMutex, std::try_to_lock);
    if (!state.owns_lock() || !impl_->capturing.load()) { impl_->drop("phase skipped while state busy"); return; }
    if (impl_->captureRecords >= kMaxRecordsPerCapture - impl_->remainingPost ||
        impl_->reservedBytes.load() >= kMaxSessionBytes) {
        impl_->drop("capture record/size limit reached"); return;
    }
    Impl::Record rec;
    rec.kind = "phase"; rec.phase.assign(phase.substr(0, 80));
    rec.snapshot = std::move(snapshot); rec.metadata = std::move(metadata);
    if (impl_->attachmentWritten) rec.metadata.attachments.clear();
    else if (!rec.metadata.attachments.empty()) impl_->attachmentWritten = true;
    rec.capture = impl_->currentCapture; rec.sequence = impl_->recordSequence++;
    ++impl_->captureRecords;
    impl_->enqueue(std::move(rec));
}

bool LiveDebugSession::isCapturing() const noexcept { return impl_->capturing.load(); }
std::uint64_t LiveDebugSession::droppedSnapshots() const noexcept { return impl_->dropped.load(); }
bool LiveDebugSession::hasError() const noexcept { return !impl_->lastError().empty(); }
std::string LiveDebugSession::lastError() const { return impl_->lastError(); }
const std::filesystem::path& LiveDebugSession::directory() const noexcept { return impl_->base; }
void LiveDebugSession::flush() { impl_->flush(); }

} // namespace saturn::tools
