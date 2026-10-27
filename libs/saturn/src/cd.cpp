#include "saturn/saturn/cd.hpp"

#include "saturn/sir/interpreter.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <string>
#include <utility>

namespace saturn::platform {
namespace {

[[nodiscard]] std::uint32_t widthMask(std::uint8_t width) noexcept {
    return width == 1U ? 0xFFU : width == 2U ? 0xFFFFU : 0xFFFFFFFFU;
}

[[nodiscard]] std::uint16_t lowWord(std::uint32_t value) noexcept {
    return static_cast<std::uint16_t>(value & 0xFFFFU);
}

[[nodiscard]] std::uint32_t readLe32(const std::vector<std::byte>& bytes,
                                     std::size_t offset) noexcept {
    if (offset + 4U > bytes.size()) return 0U;
    return std::to_integer<std::uint8_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2U])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 3U])) << 24U);
}

void appendBe32(std::vector<std::byte>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::byte>(value >> 24U));
    bytes.push_back(static_cast<std::byte>(value >> 16U));
    bytes.push_back(static_cast<std::byte>(value >> 8U));
    bytes.push_back(static_cast<std::byte>(value));
}

[[nodiscard]] std::uint8_t toBcd(std::uint32_t value) noexcept {
    value %= 100U;
    return static_cast<std::uint8_t>(((value / 10U) << 4U) | (value % 10U));
}

[[nodiscard]] std::string normalizedName(std::string_view name) {
    std::string result;
    result.reserve(name.size());
    for (const auto character : name) {
        if (character == ';') break;
        result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
    }
    while (!result.empty() && result.back() == '.') result.pop_back();
    return result;
}

} // namespace

std::optional<std::uint32_t> CdRuntime::read(core::Address address,
                                             std::uint8_t width) {
    if (width != 1U && width != 2U && width != 4U) {
        return 0U;
    }
    const auto offset = static_cast<std::uint32_t>(address - kBase) & 0xFFFFFU;
    const auto isFifo = offset == 0x18000U ||
                        (offset == 0x98000U && width == 2U);
    if (isFifo) {
        ++stats_.fifoReads;
        std::uint32_t value{};
        for (std::uint8_t index = 0; index < width; ++index) {
            const auto position = static_cast<std::size_t>(fifoOffset_) + index;
            const auto byte = position < transfer_.size()
                ? std::to_integer<std::uint8_t>(transfer_[position])
                : 0U;
            value = (value << 8U) | byte;
        }
        fifoOffset_ = std::min<std::uint32_t>(
            static_cast<std::uint32_t>(transfer_.size()),
            fifoOffset_ + width);
        transferWordsRead_ += width / 2U;
        if (fifoOffset_ >= transfer_.size()) {
            // DRDY describes the host transfer window.  CSCT is different:
            // it is the level indication that a sector is still retained in
            // a selector partition.  Yabause keeps CSCT asserted after the
            // FIFO is drained and clears it only when 0x62/0x63 (or a
            // selector reset) actually releases the retained block.
            hirq_ &= static_cast<std::uint16_t>(~kHirqDrdy);
        }
        return value;
    }
    if ((offset & 1U) != 0U && width != 1U) {
        return 0U;
    }
    if (width == 1U) {
        const auto word = readWord(offset & ~1U);
        return (address & 1U) == 0U ? (word >> 8U) : (word & 0xFFU);
    }
    const auto word = readWord(offset);
    return width == 2U ? word : (static_cast<std::uint32_t>(word) << 16U) | word;
}

bool CdRuntime::write(core::Address address, std::uint8_t width,
                      std::uint32_t value) {
    if (width != 1U && width != 2U && width != 4U) {
        return true;
    }
    value &= widthMask(width);
    const auto offset = static_cast<std::uint32_t>(address - kBase) & 0xFFFFFU;
    const auto isFifo = offset == 0x18000U ||
                        (offset == 0x98000U && width == 2U);
    if (isFifo) {
        ++stats_.fifoWrites;
        if (transfer_.empty()) {
            transfer_.resize(sectorSize_, std::byte{0});
        }
        for (std::uint8_t index = 0; index < width; ++index) {
            const auto position = static_cast<std::size_t>(fifoOffset_) + index;
            if (position >= transfer_.size()) {
                break;
            }
            const auto shift = static_cast<unsigned>((width - index - 1U) * 8U);
            transfer_[position] = static_cast<std::byte>((value >> shift) & 0xFFU);
        }
        fifoOffset_ = std::min<std::uint32_t>(
            static_cast<std::uint32_t>(transfer_.size()), fifoOffset_ + width);
        return true;
    }
    if (width == 1U) {
        const auto aligned = offset & ~1U;
        auto word = readWord(aligned);
        if ((address & 1U) == 0U) {
            word = static_cast<std::uint16_t>((word & 0x00FFU) | (value << 8U));
        } else {
            word = static_cast<std::uint16_t>((word & 0xFF00U) | value);
        }
        writeWord(aligned, word);
        return true;
    }
    if ((offset & 1U) != 0U) {
        return true;
    }
    if (width == 2U) {
        writeWord(offset, lowWord(value));
    } else {
        writeWord(offset, static_cast<std::uint16_t>(value >> 16U));
    }
    return true;
}

bool CdRuntime::setImage(std::span<const std::byte> image, std::size_t sectorSize) {
    if (sectorSize == 0U || image.size() % sectorSize != 0U) {
        return false;
    }
    return setImage(std::vector<std::byte>(image.begin(), image.end()), sectorSize);
}

bool CdRuntime::setImage(std::vector<std::byte> image, std::size_t sectorSize) {
    if (sectorSize == 0U || image.size() % sectorSize != 0U) {
        return false;
    }
    image_ = std::move(image);
    commandPending_ = false;
    imagePath_.clear();
    imageFileSize_ = 0U;
    sectorSize_ = sectorSize;
    transfer_.clear();
    fifoOffset_ = 0;
    transferWordsRead_ = 0;
    transferActive_ = false;
    subcodeActive_ = false;
    getSectorLength_ = 0U;
    putSectorLength_ = 0U;
    cdDeviceConnection_ = 0xFFU;
    lastBufferDestination_ = 0xFFU;
    directoryFilter_ = 0U;
    directoryFileId_ = 0U;
    partitionSectorCounts_.fill(0U);
    partitionStartLba_.fill(0U);
    streamingPartition_ = 0xFFU;
    streamingFilter_ = 0xFFU;
    streamingCursor_ = 0U;
    streamingSectorCount_ = 0U;
    calculatedTransferWords_ = 0U;
    currentFad_ = 150U;
    for (std::size_t index = 0U; index < filters_.size(); ++index) {
        filters_[index] = FilterState{
            0U, 0xFFFFFFFFU, 0U, static_cast<std::uint8_t>(index), 0xFFU,
            0U, 0U, 0U, 0U, 0U, 0U};
    }
    status_ = 2U;
    // A mounted ISO has a valid retained root directory. CMOK says that the
    // command block is ready, EHST lets Sega's CDC library submit the first
    // host transfer, and EFLS says that the filesystem is available.
    // Selector/history/error factors are edge-latched events and must not be
    // asserted merely because the host mounted an image. In particular,
    // raising SCDQ at boot traps BIOS code in its status-poll loop before it
    // can submit the first directory command.
    hirq_ = static_cast<std::uint16_t>(kHirqCmok | kHirqEsel | kHirqEhst |
                                       kHirqEfls);
    return true;
}

bool CdRuntime::setImageFile(const std::filesystem::path& path,
                             std::size_t sectorSize) {
    if (sectorSize == 0U) return false;
    std::error_code error;
    const auto fileSize = std::filesystem::file_size(path, error);
    if (error || fileSize == 0U || fileSize % sectorSize != 0U) return false;

    std::ifstream input(path, std::ios::binary);
    if (!input) return false;

    image_.clear();
    imagePath_ = path;
    commandPending_ = false;
    imageFileSize_ = fileSize;
    sectorSize_ = sectorSize;
    transfer_.clear();
    fifoOffset_ = 0U;
    transferWordsRead_ = 0U;
    transferActive_ = false;
    subcodeActive_ = false;
    getSectorLength_ = 0U;
    putSectorLength_ = 0U;
    cdDeviceConnection_ = 0xFFU;
    lastBufferDestination_ = 0xFFU;
    directoryFilter_ = 0U;
    directoryFileId_ = 0U;
    partitionSectorCounts_.fill(0U);
    partitionStartLba_.fill(0U);
    streamingPartition_ = 0xFFU;
    streamingFilter_ = 0xFFU;
    streamingCursor_ = 0U;
    streamingSectorCount_ = 0U;
    calculatedTransferWords_ = 0U;
    currentFad_ = 150U;
    for (std::size_t index = 0U; index < filters_.size(); ++index) {
        filters_[index] = FilterState{
            0U, 0xFFFFFFFFU, 0U, static_cast<std::uint8_t>(index), 0xFFU,
            0U, 0U, 0U, 0U, 0U, 0U};
    }
    status_ = 2U;
    hirq_ = static_cast<std::uint16_t>(kHirqCmok | kHirqEsel | kHirqEhst |
                                       kHirqEfls);
    return true;
}

void CdRuntime::clearImage() noexcept {
    commandPending_ = false;
    image_.clear();
    imagePath_.clear();
    imageFileSize_ = 0U;
    transfer_.clear();
    fifoOffset_ = 0;
    transferWordsRead_ = 0;
    transferActive_ = false;
    subcodeActive_ = false;
    cdDeviceConnection_ = 0xFFU;
    lastBufferDestination_ = 0xFFU;
    directoryFilter_ = 0U;
    directoryFileId_ = 0U;
    partitionSectorCounts_.fill(0U);
    partitionStartLba_.fill(0U);
    streamingPartition_ = 0xFFU;
    streamingFilter_ = 0xFFU;
    streamingCursor_ = 0U;
    streamingSectorCount_ = 0U;
    calculatedTransferWords_ = 0U;
    status_ = 7U;
    hirq_ = kHirqCmok;
}

bool CdRuntime::hasImage() const noexcept {
    return !image_.empty() || imageFileSize_ != 0U;
}
std::size_t CdRuntime::sectorSize() const noexcept { return sectorSize_; }
std::size_t CdRuntime::sectorCount() const noexcept {
    if (sectorSize_ == 0U) return 0U;
    if (imageFileSize_ != 0U) return static_cast<std::size_t>(imageFileSize_ / sectorSize_);
    return image_.size() / sectorSize_;
}

std::optional<std::vector<std::byte>> CdRuntime::readImageRange(
    std::uint64_t offset, std::size_t length) const {
    if (length == 0U) return std::vector<std::byte>{};
    const auto total = imageFileSize_ != 0U
        ? imageFileSize_ : static_cast<std::uint64_t>(image_.size());
    if (offset > total || length > total - offset) return std::nullopt;

    std::vector<std::byte> result(length);
    if (imageFileSize_ == 0U) {
        std::copy_n(image_.begin() + static_cast<std::ptrdiff_t>(offset),
                    static_cast<std::ptrdiff_t>(length), result.begin());
        return result;
    }

    std::ifstream input(imagePath_, std::ios::binary);
    if (!input) return std::nullopt;
    input.seekg(static_cast<std::streamoff>(offset));
    if (!input) return std::nullopt;
    input.read(reinterpret_cast<char*>(result.data()),
               static_cast<std::streamsize>(length));
    if (!input) return std::nullopt;
    return result;
}

std::optional<std::vector<std::byte>> CdRuntime::readSector(std::uint32_t lba) const {
    if (static_cast<std::size_t>(lba) >= sectorCount()) {
        return std::nullopt;
    }
    return readImageRange(static_cast<std::uint64_t>(lba) * sectorSize_, sectorSize_);
}

std::optional<std::vector<std::byte>> CdRuntime::readDataSector(std::uint32_t lba) const {
    const auto sector = readSector(lba);
    if (!sector) return std::nullopt;
    if ((sectorSize_ == 2352U || sectorSize_ == 2448U) && sector->size() >= 2064U) {
        return std::vector<std::byte>(sector->begin() + 16, sector->begin() + 2064);
    }
    if (sector->size() < kLogicalSectorSize) return sector;
    return std::vector<std::byte>(sector->begin(), sector->begin() + kLogicalSectorSize);
}

std::optional<std::vector<std::byte>> CdRuntime::readBytes(std::uint64_t offset,
                                                           std::size_t length) const {
    if (length == 0U) return std::vector<std::byte>{};
    const auto end = offset + length;
    if (end < offset || end > static_cast<std::uint64_t>(sectorCount()) *
                               kLogicalSectorSize) {
        return std::nullopt;
    }
    std::vector<std::byte> result;
    result.reserve(length);
    while (result.size() < length) {
        const auto absolute = offset + result.size();
        const auto lba = static_cast<std::uint32_t>(absolute / kLogicalSectorSize);
        const auto inSector = static_cast<std::size_t>(absolute % kLogicalSectorSize);
        const auto sector = readDataSector(lba);
        if (!sector) return std::nullopt;
        const auto copy = std::min(length - result.size(), kLogicalSectorSize - inSector);
        result.insert(result.end(), sector->begin() + static_cast<std::ptrdiff_t>(inSector),
                      sector->begin() + static_cast<std::ptrdiff_t>(inSector + copy));
    }
    return result;
}

std::optional<CdRuntime::FileEntry> CdRuntime::findDirectoryEntry(
    std::uint32_t lba, std::uint32_t size, std::string_view component) const {
    const auto bytes = readBytes(static_cast<std::uint64_t>(lba) * kLogicalSectorSize, size);
    if (!bytes) return std::nullopt;
    const auto wanted = normalizedName(component);
    std::size_t offset = 0;
    while (offset < bytes->size()) {
        const auto recordLength = std::to_integer<std::uint8_t>((*bytes)[offset]);
        if (recordLength == 0U) {
            offset = ((offset / kLogicalSectorSize) + 1U) * kLogicalSectorSize;
            continue;
        }
        if (recordLength < 34U || offset + recordLength > bytes->size()) break;
        const auto nameLength = std::to_integer<std::uint8_t>((*bytes)[offset + 32U]);
        if (33U + nameLength <= recordLength && nameLength != 1U) {
            std::string name;
            name.reserve(nameLength);
            for (std::size_t index = 0; index < nameLength; ++index) {
                name.push_back(static_cast<char>(std::to_integer<std::uint8_t>(
                    (*bytes)[offset + 33U + index])));
            }
            if (normalizedName(name) == wanted) {
                return FileEntry{readLe32(*bytes, offset + 2U),
                                 readLe32(*bytes, offset + 10U),
                                 (std::to_integer<std::uint8_t>((*bytes)[offset + 25U]) & 2U) != 0U};
            }
        }
        offset += recordLength;
    }
    return std::nullopt;
}

std::size_t CdRuntime::rootFileCount() const {
    return rootFileInfo().size();
}

std::vector<CdRuntime::RetainedFileInfo> CdRuntime::rootFileInfo() const {
    const auto primary = readDataSector(16U);
    if (!primary || primary->size() < 190U ||
        std::to_integer<std::uint8_t>((*primary)[0]) != 1U ||
        std::string_view(reinterpret_cast<const char*>(primary->data() + 1), 5) !=
            "CD001") {
        return {};
    }
    const auto rootSize = readLe32(*primary, 166U);
    const auto rootLba = readLe32(*primary, 158U);
    const auto bytes = readBytes(static_cast<std::uint64_t>(rootLba) *
                                     kLogicalSectorSize,
                                 rootSize);
    if (!bytes) return {};

    std::vector<RetainedFileInfo> result;
    std::size_t offset{};
    while (offset < bytes->size()) {
        const auto recordLength = std::to_integer<std::uint8_t>((*bytes)[offset]);
        if (recordLength == 0U) {
            offset = ((offset / kLogicalSectorSize) + 1U) * kLogicalSectorSize;
            continue;
        }
        if (recordLength < 34U || offset + recordLength > bytes->size()) break;
        const auto nameLength = std::to_integer<std::uint8_t>((*bytes)[offset + 32U]);
        const auto special = nameLength == 1U &&
            (std::to_integer<std::uint8_t>((*bytes)[offset + 33U]) == 0U ||
             std::to_integer<std::uint8_t>((*bytes)[offset + 33U]) == 1U);
        if (!special) {
            result.push_back(RetainedFileInfo{
                readLe32(*bytes, offset + 2U) + 150U,
                readLe32(*bytes, offset + 10U),
                std::to_integer<std::uint8_t>((*bytes)[offset + 26U]),
                std::to_integer<std::uint8_t>((*bytes)[offset + 27U]),
                static_cast<std::uint8_t>(result.size() + 2U),
                static_cast<std::uint8_t>(
                    std::to_integer<std::uint8_t>((*bytes)[offset + 25U]) & 2U)});
        }
        offset += recordLength;
    }
    return result;
}

std::vector<std::byte> CdRuntime::encodeFileInfo(std::uint32_t fileId) const {
    const auto entries = rootFileInfo();
    std::vector<std::byte> bytes;
    const auto append = [&bytes](const RetainedFileInfo& entry) {
        appendBe32(bytes, entry.fad);
        appendBe32(bytes, entry.size);
        bytes.push_back(static_cast<std::byte>(entry.gapSize));
        bytes.push_back(static_cast<std::byte>(entry.unitSize));
        bytes.push_back(static_cast<std::byte>(entry.fileNumber));
        bytes.push_back(static_cast<std::byte>(entry.attributes));
    };

    if (fileId == 0xFFFFFFU) {
        bytes.reserve(entries.size() * 12U);
        for (const auto& entry : entries) append(entry);
    } else if (fileId >= 2U && fileId - 2U < entries.size()) {
        bytes.reserve(12U);
        append(entries[fileId - 2U]);
    }
    return bytes;
}

std::vector<std::byte> CdRuntime::encodeToc() const {
    constexpr std::size_t kTocEntries = 102U;
    std::vector<std::uint32_t> entries(kTocEntries, 0xFFFFFFFFU);
    // A mounted raw image is exposed as one Mode-1 data track. Saturn's TOC
    // words store CTRL/ADR in the high byte and a 24-bit FAD in the low part.
    constexpr std::uint32_t kDataTrack = 0x41000000U;
    entries[0] = kDataTrack | 150U;
    entries[99] = kDataTrack | 0x00010000U; // A0: first track is 1
    entries[100] = kDataTrack | 0x00010000U; // A1: last track is 1
    const auto leadOut = std::min<std::size_t>(
        sectorCount() + 150U, 0x00FFFFFFU);
    entries[101] = kDataTrack | static_cast<std::uint32_t>(leadOut);

    std::vector<std::byte> bytes;
    bytes.reserve(kTocEntries * 4U);
    for (const auto entry : entries) appendBe32(bytes, entry);
    return bytes;
}

std::optional<CdRuntime::FileEntry> CdRuntime::findFile(std::string_view path) const {
    const auto primary = readDataSector(16U);
    if (!primary || primary->size() < 190U ||
        std::to_integer<std::uint8_t>((*primary)[0]) != 1U ||
        std::string_view(reinterpret_cast<const char*>(primary->data() + 1), 5) != "CD001") {
        return std::nullopt;
    }
    const auto rootSize = readLe32(*primary, 166U);
    const auto rootLba = readLe32(*primary, 158U);
    std::size_t begin = 0;
    while (begin < path.size() && (path[begin] == '/' || path[begin] == '\\')) ++begin;
    if (begin == path.size()) return FileEntry{rootLba, rootSize, true};

    std::uint32_t directoryLba = rootLba;
    std::uint32_t directorySize = rootSize;
    while (begin < path.size()) {
        const auto slash = path.find_first_of("/\\", begin);
        const auto component = path.substr(begin, slash == std::string_view::npos
                                                     ? path.size() - begin
                                                     : slash - begin);
        if (component.empty()) {
            begin = slash == std::string_view::npos ? path.size() : slash + 1U;
            continue;
        }
        const auto entry = findDirectoryEntry(directoryLba, directorySize, component);
        if (!entry) return std::nullopt;
        if (slash == std::string_view::npos) return entry;
        if (!entry->directory) return std::nullopt;
        directoryLba = entry->lba;
        directorySize = entry->size;
        begin = slash + 1U;
    }
    return std::nullopt;
}

std::optional<std::vector<std::byte>> CdRuntime::readFile(std::string_view path) const {
    const auto entry = findFile(path);
    if (!entry || entry->directory) return std::nullopt;
    return readBytes(static_cast<std::uint64_t>(entry->lba) * kLogicalSectorSize, entry->size);
}

std::optional<CdRuntime::DmaTransfer> CdRuntime::dmaReadFile(
    std::string_view path, core::Address destination, sir::MachineState& state) {
    const auto entry = findFile(path);
    if (!entry || entry->directory) return std::nullopt;

    DmaTransfer result{destination, entry->size,
                       0U, (static_cast<std::size_t>(entry->size) +
                             kLogicalSectorSize - 1U) / kLogicalSectorSize, false};
    for (std::size_t sector = 0; sector < result.sectors; ++sector) {
        const auto lba = static_cast<std::uint64_t>(entry->lba) + sector;
        if (lba > 0xFFFFFFFFULL) return std::nullopt;

        // The convenience helper already resolved the ISO file to LBAs. Feed
        // those sectors through the same FIFO state as a host transfer; the
        // real 0x61 command addresses retained partition sectors, not LBAs.
        prepareSector(static_cast<std::uint32_t>(lba));
        if (!transferActive_) return std::nullopt;

        const auto remaining = static_cast<std::size_t>(entry->size) - result.transferred;
        const auto sectorBytes = std::min(remaining, kLogicalSectorSize);
        std::size_t consumed = 0U;
        while (consumed < kLogicalSectorSize) {
            const auto width = static_cast<std::uint8_t>(
                std::min<std::size_t>(4U, kLogicalSectorSize - consumed));
            const auto value = read(kFifo, width);
            if (!value) return std::nullopt;
            for (std::uint8_t index = 0; index < width; ++index) {
                if (consumed + index < sectorBytes) {
                    const auto shift = static_cast<unsigned>((width - index - 1U) * 8U);
                    if (result.transferred >
                        static_cast<std::size_t>(std::numeric_limits<core::Address>::max()) -
                            static_cast<std::size_t>(destination)) {
                        return std::nullopt;
                    }
                    const auto address = static_cast<core::Address>(
                        destination + result.transferred);
                    state.write8(address,
                                 static_cast<std::uint8_t>(*value >> shift));
                    ++result.transferred;
                }
            }
            consumed += width;
        }

        // A short final file sector is padded by the sector read, so the DMA
        // sink drains the complete FIFO before returning the CD Block to its
        // command-ready state.
        if (!write(kCommandBase + 0x18U, 2U, 0x0600U) ||
            !write(kCommandBase + 0x1CU, 2U, 0U) ||
            !write(kCommandBase + 0x20U, 2U, 0U) ||
            !write(kCommandBase + 0x24U, 2U, 0U)) {
            return std::nullopt;
        }
    }
    result.complete = result.transferred == result.requested;
    return result;
}

std::uint16_t CdRuntime::hirq() const noexcept { return visibleHirq(); }
std::uint16_t CdRuntime::hirqMask() const noexcept { return hirqMask_; }
std::uint16_t CdRuntime::commandRegister(std::size_t index) const noexcept {
    return index < 4U ? command_[index] : 0U;
}
std::uint32_t CdRuntime::fifoOffset() const noexcept { return fifoOffset_; }

bool CdRuntime::fifoDataAvailable() const noexcept {
    return transferActive_ && fifoOffset_ < transfer_.size();
}

bool CdRuntime::subcodeActive() const noexcept { return subcodeActive_; }

const CdRuntime::Stats& CdRuntime::stats() const noexcept { return stats_; }

void CdRuntime::setSectorTraceEnabled(bool enabled) {
    sectorTraceEnabled_ = enabled;
    if (!enabled) return;
    playTrace_.clear();
    sectorTrace_.clear();
    playTraceCount_ = 0U;
    sectorTraceCount_ = 0U;
    droppedPlayTraceRecords_ = 0U;
    droppedSectorTraceRecords_ = 0U;
    try {
        playTrace_.reserve(4096U);
        sectorTrace_.reserve(65536U);
    } catch (...) {
        // The trace remains best-effort if the diagnostic allocation fails.
    }
}

const std::vector<CdRuntime::PlayTraceRecord>&
CdRuntime::playTrace() const noexcept {
    return playTrace_;
}

const std::vector<CdRuntime::SectorTraceRecord>&
CdRuntime::sectorTrace() const noexcept {
    return sectorTrace_;
}

std::uint64_t CdRuntime::droppedPlayTraceRecords() const noexcept {
    return droppedPlayTraceRecords_;
}

std::uint64_t CdRuntime::droppedSectorTraceRecords() const noexcept {
    return droppedSectorTraceRecords_;
}

void CdRuntime::recordPlayTrace(const PlayTraceRecord& record) noexcept {
    if (!sectorTraceEnabled_) return;
    auto retained = record;
    retained.sequence = ++playTraceCount_;
    if (playTrace_.size() >= 1'000'000U) {
        ++droppedPlayTraceRecords_;
        return;
    }
    try {
        playTrace_.push_back(retained);
    } catch (...) {
        ++droppedPlayTraceRecords_;
    }
}

void CdRuntime::recordSectorTrace(std::uint32_t fad,
                                  std::uint32_t playCursor,
                                  std::uint8_t partition,
                                  std::uint8_t source,
                                  bool retained) noexcept {
    if (!sectorTraceEnabled_) return;
    const auto sequence = ++sectorTraceCount_;
    if (sectorTrace_.size() >= 1'000'000U) {
        ++droppedSectorTraceRecords_;
        return;
    }
    try {
        sectorTrace_.push_back({sequence, fad, fad >= 150U ? fad - 150U : 0U,
                                playCursor, partition, source, retained});
    } catch (...) {
        ++droppedSectorTraceRecords_;
    }
}

std::uint8_t CdRuntime::streamingDestination() const noexcept {
    auto filter = streamingFilter_;
    for (std::size_t depth = 0U; depth < filters_.size(); ++depth) {
        if (filter >= filters_.size()) return 0xFFU;
        const auto& state = filters_[filter];
        // The current corpus is a MODE-1 data track, so XA subheader
        // predicates are vacuously true just as in Yabause's FilterData.
        const auto inRange = (state.mode & 0x40U) == 0U ||
            (currentFad_ >= state.fad &&
             currentFad_ < state.fad + state.range);
        if (inRange) return state.trueConnection;
        filter = state.falseConnection;
        if (filter == 0xFFU) return 0xFFU;
    }
    return 0xFFU;
}

void CdRuntime::recordStreamingFilter(std::uint8_t destination) noexcept {
    stats_.lastStreamingFilter = streamingFilter_;
    stats_.lastStreamingDestination = destination;
    if (streamingFilter_ >= filters_.size()) {
        stats_.lastFilterFad = 0U;
        stats_.lastFilterRange = 0U;
        stats_.lastFilterMode = 0U;
        stats_.lastFilterTrueConnection = 0xFFU;
        stats_.lastFilterFalseConnection = 0xFFU;
        return;
    }
    const auto& state = filters_[streamingFilter_];
    stats_.lastFilterFad = state.fad;
    stats_.lastFilterRange = state.range;
    stats_.lastFilterMode = state.mode;
    stats_.lastFilterTrueConnection = state.trueConnection;
    stats_.lastFilterFalseConnection = state.falseConnection;
}

void CdRuntime::onSubcodeTick(std::size_t sectorBudget) noexcept {
    ++stats_.subcodeTicks;
    stats_.lastSubcodeActive = subcodeActive_;
    stats_.lastTransferActive = transferActive_;
    if (!hasImage()) return;
    // A positional SEEK finishes at PAUSE. Keep its synchronous reply
    // intact below until CR4 is read, even if the seek has already finished.
    if (status_ == 4U) status_ = 1U;
    if (!subcodeActive_) ++stats_.subcodeInactiveTicks;
    else if (streamingCursor_ >= streamingSectorCount_) ++stats_.subcodeRangeCompleteTicks;
    // Data reads run at 2x (150 sectors/s), while reports run at 75 Hz.
    // Each sector remains in the selected CD buffer
    // partition until a 0x61/0x63 host-transfer command exposes it through
    // the FIFO; retaining a sector alone must not assert DRDY. The drive
    // continues filling buffer RAM while the host drains its separate FIFO
    // snapshot, otherwise streaming PCM can replay an unrefilled ring.
    for (std::size_t sector = 0; sector < sectorBudget && subcodeActive_ &&
        streamingCursor_ < streamingSectorCount_; ++sector) {
        streamingPartition_ = streamingDestination();
        recordStreamingFilter(streamingPartition_);
        if (streamingPartition_ < partitionSectorCounts_.size() &&
            freeBufferSectors() != 0U) {
            recordSectorTrace(currentFad_, streamingCursor_, streamingPartition_,
                              0U, true);
            lastBufferDestination_ = streamingPartition_;
            if (partitionSectorCounts_[streamingPartition_] == 0U) {
                partitionStartLba_[streamingPartition_] =
                    currentFad_ >= 150U ? currentFad_ - 150U : 0U;
            }
            ++streamingCursor_;
            ++partitionSectorCounts_[streamingPartition_];
            ++stats_.subcodeProducedTicks;
            stats_.lastPartition = streamingPartition_;
            stats_.lastPartitionSectorCount = partitionSectorCounts_[streamingPartition_];
            stats_.lastPlayPartitionCountAfterTick =
                partitionSectorCounts_[streamingPartition_];
            stats_.lastStreamingCursor = streamingCursor_;
            stats_.lastStreamingSectorCount = streamingSectorCount_;
            ++currentFad_;
            hirq_ |= kHirqCsct;
        } else if (streamingPartition_ >= partitionSectorCounts_.size()) {
            recordSectorTrace(currentFad_, streamingCursor_, streamingPartition_,
                              0U, false);
            ++stats_.subcodeInvalidDestinationTicks;
            // A false filter chain can intentionally discard a sector.  The
            // drive still advances through the requested PLAY range and only
            // raises CSCT when a partition actually receives the sector.
            ++streamingCursor_;
            ++currentFad_;
        } else {
            ++stats_.subcodeBackpressureTicks;
        }
        if (streamingCursor_ >= streamingSectorCount_) {
            // The drive enters PAUSE after the requested play range has been
            // retained and publishes the play-end factor. GFS waits for this
            // transition before requesting the retained sector data.
            status_ = 1U;
            subcodeActive_ = false;
            hirq_ |= kHirqPend;
        }
    }
    // Periodic subcode reports carry the PERI qualifier in the status byte;
    // synchronous command responses keep the plain four-bit drive status.
    // Build the report after the producer step so a sector that reaches the
    // end of its PLAY range reports PAUSE (as Yabause does), rather than one
    // stale PLAY packet followed by a separate status poll.
    // Sector production continues during commands, but the periodic report
    // must not replace a packet being submitted or an unread command reply.
    // Reading CR4 acknowledges the reply, matching the CD register handshake.
    if (commandPending_) return;
    command_[0] = static_cast<std::uint16_t>((status_ | 0x20U) << 8U);
    // Periodic status layout is CR2=(CTRL/ADR << 8)|track and
    // CR3=(index << 8)|(FAD >> 16).
    command_[1] = 0x4101U; // data track (CTRL/ADR 1), track 1
    command_[2] = static_cast<std::uint16_t>(0x0100U | (currentFad_ >> 16U));
    command_[3] = static_cast<std::uint16_t>(currentFad_);
    hirq_ |= kHirqScdq;
}

void CdRuntime::prepareSector(std::uint32_t lba) {
    ++stats_.sectorsPrepared;
    currentFad_ = lba + 150U;
    transfer_ = encodeSectorForTransfer(lba);
    fifoOffset_ = 0;
    transferWordsRead_ = 0;
    transferActive_ = !transfer_.empty();
    recordSectorTrace(lba + 150U, streamingCursor_, lastBufferDestination_,
                      1U, transferActive_);
    if (!transfer_.empty()) {
        hirq_ |= static_cast<std::uint16_t>(kHirqCsct | kHirqDrdy);
    } else {
        hirq_ &= static_cast<std::uint16_t>(~(kHirqCsct | kHirqDrdy));
    }
}

std::vector<std::byte> CdRuntime::encodeSectorForTransfer(std::uint32_t lba) const {
    const auto sector = readSector(lba);
    if (!sector) return {};

    // The CD block's get-sector-length codes expose 2048, 2336, 2340 or
    // 2352 bytes. Raw MODE1 images already contain every representation; a
    // cooked ISO can only provide its 2048-byte user-data portion.
    if (sector->size() >= 2352U) {
        constexpr std::array<std::size_t, 4> kOffsets{16U, 16U, 12U, 0U};
        constexpr std::array<std::size_t, 4> kSizes{2048U, 2336U, 2340U, 2352U};
        const auto offset = kOffsets[getSectorLength_];
        const auto size = kSizes[getSectorLength_];
        return std::vector<std::byte>(
            sector->begin() + static_cast<std::ptrdiff_t>(offset),
            sector->begin() + static_cast<std::ptrdiff_t>(offset + size));
    }
    return readDataSector(lba).value_or(std::vector<std::byte>{});
}

std::uint16_t CdRuntime::readWord(std::uint32_t offset) {
    switch (offset) {
    case 0x90008U:
    case 0x9000AU:
        ++stats_.hirqReads;
        // CSCT is a level indication, not a one-shot edge.  The real CD
        // block derives it from ``isonesectorstored`` every time HIRQ is
        // read; keeping only the latched bit lets a command acknowledgement
        // leave the guest polling a stale value after the retained partition
        // changes.  Mirror that level from the partition model here.
        if (std::any_of(
                partitionSectorCounts_.begin(), partitionSectorCounts_.end(),
                [](std::uint16_t sectors) { return sectors != 0U; })) {
            hirq_ |= kHirqCsct;
        } else {
            hirq_ &= static_cast<std::uint16_t>(~kHirqCsct);
        }
        // CMOK is the command-ready level, not a consumable edge.  The BIOS
        // clears it before submitting a command and then polls until the CD
        // block raises it again.  Reasserting it for an already-completed HLE
        // operation prevents a synchronous command response from leaving the
        // poller in a permanent busy state while DRDY/EHST remain latched.
        if (hasImage()) hirq_ |= kHirqCmok;
        stats_.lastHirq = visibleHirq();
        for (std::size_t factor = 0U; factor < stats_.hirqReadFactors.size(); ++factor) {
            if ((stats_.lastHirq & (1U << factor)) != 0U) {
                ++stats_.hirqReadFactors[factor];
            }
        }
        return stats_.lastHirq;
    case 0x9000CU:
    case 0x9000EU: return hirqMask_;
    case 0x90018U:
    case 0x9001AU: return command_[0];
    case 0x9001CU:
    case 0x9001EU: return command_[1];
    case 0x90020U:
    case 0x90022U: return command_[2];
    case 0x90024U:
    case 0x90026U:
        commandPending_ = false;
        return command_[3];
    case 0x90028U:
    case 0x9002AU: return mpegRgb_;
    default: return 0U;
    }
}

void CdRuntime::writeWord(std::uint32_t offset, std::uint16_t value) {
    switch (offset) {
    case 0x90008U:
    case 0x9000AU:
        ++stats_.hirqWrites;
        stats_.hirqWriteHistory[stats_.hirqWriteHistoryCount %
                                 stats_.hirqWriteHistory.size()] = value;
        ++stats_.hirqWriteHistoryCount;
        hirq_ &= value;
        break;
    case 0x9000CU:
    case 0x9000EU:
        hirqMask_ = static_cast<std::uint16_t>(value & 0x1FFFU);
        break;
    case 0x90018U:
    case 0x9001AU:
        commandPending_ = true;
        command_[0] = value;
        status_ = static_cast<std::uint8_t>(status_ & 0x0FU);
        // SCDQ is acknowledged through HIRQ, independently of CR1. GFS uses
        // that latched event to advance its streaming position even while it
        // issues sector-count commands. The packet remains protected by
        // commandPending_; CDC rejects non-PERI replies when reading status.
        break;
    case 0x9001CU:
    case 0x9001EU: command_[1] = value; break;
    case 0x90020U:
    case 0x90022U: command_[2] = value; break;
    case 0x90024U:
    case 0x90026U:
        command_[3] = value;
        executeCommand();
        break;
    case 0x90028U:
    case 0x9002AU: mpegRgb_ = value; break;
    default: break;
    }
}

std::uint16_t CdRuntime::visibleHirq() const noexcept {
    // All selectors share the same 200-sector buffer. BFUL is a level,
    // including after the guest acknowledges HIRQ or deletes retained data.
    return static_cast<std::uint16_t>((hirq_ & ~kHirqBful) |
        (freeBufferSectors() == 0U ? kHirqBful : 0U));
}

std::uint16_t CdRuntime::freeBufferSectors() const noexcept {
    std::uint32_t retained = 0U;
    for (const auto count : partitionSectorCounts_) retained += count;
    return static_cast<std::uint16_t>(200U - std::min(retained, 200U));
}

void CdRuntime::executeCommand() {
    ++stats_.commands;
    hirq_ &= static_cast<std::uint16_t>(~kHirqCmok);
    const auto instruction = static_cast<std::uint8_t>(command_[0] >> 8U);
    stats_.lastInstruction = instruction;
    ++stats_.instructionCounts[instruction];
    if (instruction == 0x10U) {
        stats_.lastPlayRegisters = {command_[0], command_[1], command_[2], command_[3]};
        stats_.playHistory[stats_.playHistoryCount % stats_.playHistory.size()] =
            {command_[0], command_[1], command_[2], command_[3]};
        ++stats_.playHistoryCount;
        ++stats_.playCommands;
    }
    stats_.commandHistory[stats_.commandHistoryCount % stats_.commandHistory.size()] = instruction;
    stats_.commandRegistersHistory[stats_.commandHistoryCount % stats_.commandRegistersHistory.size()] =
        {command_[0], command_[1], command_[2], command_[3]};
    ++stats_.commandHistoryCount;
    switch (instruction) {
    case 0x00U: // get status
        // Return the complete four-word CD status packet.  The status command
        // is sampled by the BIOS after SEEK/PLAY and expects the same
        // CTRL/ADR, track, index and FAD fields as a periodic response (with
        // the periodic flag itself clear because this is a synchronous
        // command response).
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = 0x4101U;
        command_[2] = static_cast<std::uint16_t>(0x0100U | (currentFad_ >> 16U));
        command_[3] = static_cast<std::uint16_t>(currentFad_);
        hirq_ |= kHirqCmok;
        break;
    case 0x01U: // get hardware information
        // Match the observable response used by the Saturn BIOS/Yabause:
        // version 1 CD block, no MPEG card, and drive revision 4.00.
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = 0x0001U;
        command_[2] = 0U;
        command_[3] = 0x0400U;
        hirq_ |= kHirqCmok;
        break;
    case 0x02U: { // get complete 102-entry TOC through the host FIFO
        transfer_ = hasImage() ? encodeToc() : std::vector<std::byte>{};
        fifoOffset_ = 0U;
        transferWordsRead_ = 0U;
        transferActive_ = !transfer_.empty();
        if (!transferActive_) {
            command_[0] = 0xFF00U;
            hirq_ |= kHirqCmok;
            break;
        }
        // GET TOC returns the fixed 0xCC response selector.  The payload is
        // consumed through the FIFO while DRDY is asserted.  Unlike the
        // retained-sector commands, Yabause reports the unqualified current
        // status in CR1 for this command.
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = 0x00CCU;
        command_[2] = 0U;
        command_[3] = 0U;
        hirq_ |= static_cast<std::uint16_t>(kHirqCmok | kHirqDrdy);
        status_ = hasImage() ? 1U : 7U;
        break;
    }
    case 0x03U: { // get session information
        const auto session = static_cast<std::uint8_t>(command_[0]);
        std::uint32_t fad{};
        std::uint8_t response{};
        if (!hasImage() || session > 1U) {
            fad = 0x00FFFFFFU;
            response = 0xFFU;
        } else if (session == 0U) {
            fad = static_cast<std::uint32_t>(std::min<std::size_t>(
                sectorCount() + 150U, 0x00FFFFFFU));
            response = 1U; // one retained session
        } else {
            // Session 1 starts at the first data-track program area.
            fad = 0U;
            response = 1U;
        }
        status_ = hasImage() ? 2U : 7U;
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = 0U;
        command_[2] = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(response) << 8U) | (fad >> 16U));
        command_[3] = static_cast<std::uint16_t>(fad);
        hirq_ |= kHirqCmok;
        break;
    }
    case 0x04U: // initialize CD system
        status_ = hasImage() ? 1U : 7U;
        currentFad_ = 150U;
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = 0x4101U;
        command_[2] = 0x0100U;
        command_[3] = static_cast<std::uint16_t>(currentFad_);
        hirq_ |= static_cast<std::uint16_t>(kHirqCmok |
                                           (hasImage() ? kHirqEsel : 0U));
        break;
    case 0x05U: // open tray
        status_ = 6U;
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        hirq_ |= kHirqCmok;
        break;
    case 0x06U: // end transfer
        if (transferActive_) {
            command_[0] = static_cast<std::uint16_t>(
                (status_ << 8U) | ((transferWordsRead_ >> 16U) & 0xFFU));
            command_[1] = static_cast<std::uint16_t>(transferWordsRead_);
        } else {
            command_[0] = static_cast<std::uint16_t>((status_ << 8U) | 0xFFU);
            command_[1] = 0xFFFFU;
        }
        command_[2] = 0U;
        command_[3] = 0U;
        transfer_.clear();
        fifoOffset_ = 0;
        transferWordsRead_ = 0;
        transferActive_ = false;
        hirq_ = static_cast<std::uint16_t>(hirq_ | kHirqCmok);
        hirq_ &= static_cast<std::uint16_t>(~kHirqDrdy);
        break;
    case 0x10U: // play disc
    {
        // PLAY changes the data source from any previous filesystem/FIFO
        // transfer to the asynchronous sector producer.  Drop the stale HLE
        // transfer latch here; otherwise a guest that starts PLAY immediately
        // after draining a file can remain blocked behind an obsolete
        // transferActive_ bit and poll 0x51 forever.
        transfer_.clear();
        fifoOffset_ = 0U;
        transferWordsRead_ = 0U;
        transferActive_ = false;
        const auto rawStart =
            (static_cast<std::uint32_t>(command_[0] & 0x00FFU) << 16U) |
            command_[1];
        const auto rawEnd =
            (static_cast<std::uint32_t>(command_[2] & 0x00FFU) << 16U) |
            command_[3];
        const auto startIsFad = (rawStart & 0x00800000U) != 0U;
        const auto endIsFad = (rawEnd & 0x00800000U) != 0U;
        const auto startFad = rawStart & 0x007FFFFFU;
        auto requested = endIsFad ? rawEnd & 0x007FFFFFU : 0U;
        if (!startIsFad || startFad < 150U || !endIsFad) requested = 0U;
        const auto lba = startFad >= 150U ? startFad - 150U : 0U;
        const auto available = static_cast<std::uint32_t>(
            std::min<std::size_t>(sectorCount() > lba ? sectorCount() - lba : 0U,
                                  std::numeric_limits<std::uint32_t>::max()));
        const auto playable = std::min(requested, available);
        stats_.lastPlayRequested = requested;
        stats_.lastPlayAvailable = available;
        stats_.lastPlayRetained = static_cast<std::uint16_t>(
            std::min<std::uint32_t>(playable,
                                    std::numeric_limits<std::uint16_t>::max()));
        streamingFilter_ = cdDeviceConnection_;
        currentFad_ = startFad;
        streamingPartition_ = streamingDestination();
        stats_.lastPlayDestination = streamingPartition_;
        recordStreamingFilter(streamingPartition_);
        recordPlayTrace({0U, {command_[0], command_[1], command_[2], command_[3]},
                         startFad, startFad + requested, requested, available, playable,
                         streamingPartition_});
        if (hasImage() && streamingPartition_ < partitionSectorCounts_.size()) {
            partitionStartLba_[streamingPartition_] = lba;
            partitionSectorCounts_[streamingPartition_] = 0U;
            streamingCursor_ = 0U;
            streamingSectorCount_ = playable;
        }
        stats_.lastPartition = streamingPartition_;
        stats_.lastPartitionSectorCount =
            streamingPartition_ < partitionSectorCounts_.size()
                ? partitionSectorCounts_[streamingPartition_]
                : 0U;
        stats_.lastStreamingCursor = streamingCursor_;
        stats_.lastStreamingSectorCount = streamingSectorCount_;
        status_ = hasImage() ? 3U : 7U;
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        // PLAY returns the normal four-word drive report.  CR2 is
        // CTRL/ADR+track, CR3 is index+FAD high, and CR4 is the current FAD;
        // the retained-sector count is observed later through 0x51.
        command_[1] = 0x4101U;
        command_[2] = static_cast<std::uint16_t>(0x0100U | (startFad >> 16U));
        command_[3] = static_cast<std::uint16_t>(startFad);
        hirq_ |= kHirqCmok;
        subcodeActive_ = playable != 0U;
        break;
    }
    case 0x11U: { // seek disc; FFFFFF pauses and zero stops
        const auto target = (static_cast<std::uint32_t>(command_[0] & 0x00FFU) << 16U) |
                            command_[1];
        status_ = 7U;
        if (hasImage() && target != 0U && target != 0x00FFFFFFU) {
            currentFad_ = target & 0x007FFFFFU;
        }
        // The CD status codes are not the command numbers: PAUSE is 1,
        // STANDBY/STOP is 2, and SEEK is 4.  Rockman's recovery path waits
        // specifically for the periodic PAUSE report after issuing
        // SEEK(0xFFFFFF); returning STANDBY here leaves it polling forever.
        if (hasImage()) {
            status_ = target == 0x00FFFFFFU ? 1U :
                      target == 0U ? 2U : 4U;
        }
        // Seeking (including pause/stop) stops sector production. Periodic
        // reports are independent of this PLAY producer latch. Preserve
        // retained partitions, but never produce from the old PLAY range
        // at the new pickup position.
        subcodeActive_ = false;
        // SCDQ is generated by the periodic subcode producer, not by the
        // synchronous command completion itself.  Hardware keeps the command
        // response and periodic report phases separate; asserting SCDQ here
        // makes polling code consume CR1 while it still contains the command
        // response and falsely classify the drive as an invalid report.
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = 0U;
        command_[2] = 0U;
        command_[3] = 0U;
        hirq_ |= kHirqCmok;
        break;
    }
    case 0x20U: { // get subcode Q/RW through the host FIFO
        const auto type = static_cast<std::uint8_t>(command_[0]);
        transfer_.clear();
        if (type == 0U) {
            const auto absolute = currentFad_;
            const auto relative = absolute >= 150U ? absolute - 150U : 0U;
            const auto appendMsf = [this](std::uint32_t fad) {
                const auto minutes = fad / (75U * 60U);
                const auto seconds = (fad / 75U) % 60U;
                const auto frames = fad % 75U;
                transfer_.push_back(static_cast<std::byte>(toBcd(minutes)));
                transfer_.push_back(static_cast<std::byte>(toBcd(seconds)));
                transfer_.push_back(static_cast<std::byte>(toBcd(frames)));
            };
            transfer_.push_back(std::byte{0x41}); // data track CTRL/ADR
            transfer_.push_back(std::byte{0x01}); // track 1
            transfer_.push_back(std::byte{0x01}); // index 1
            appendMsf(relative);
            transfer_.push_back(std::byte{0});
            appendMsf(absolute);
        } else if (type == 1U) {
            // Raw subcode RW is optional in images. Return one deterministic
            // zeroed 24-byte group when the caller explicitly requests it.
            transfer_.assign(24U, std::byte{0});
        }
        fifoOffset_ = 0U;
        transferWordsRead_ = 0U;
        transferActive_ = !transfer_.empty();
        if (!transferActive_) {
            command_[0] = 0xFF00U;
            command_[1] = command_[2] = command_[3] = 0U;
            hirq_ |= kHirqCmok;
            break;
        }
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = static_cast<std::uint16_t>(transfer_.size() / 2U);
        command_[2] = 0U;
        command_[3] = 0U;
        hirq_ |= static_cast<std::uint16_t>(kHirqCmok | kHirqDrdy);
        break;
    }
    case 0x30U: { // set CD device connection
        const auto filter = static_cast<std::uint8_t>(command_[2] >> 8U);
        if (filter != 0xFFU && filter >= filters_.size()) {
            command_[0] = 0xFF00U;
            hirq_ |= kHirqCmok;
            break;
        }
        cdDeviceConnection_ = filter;
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = 0U;
        command_[2] = 0U;
        command_[3] = 0U;
        hirq_ |= static_cast<std::uint16_t>(kHirqCmok | kHirqEsel);
        break;
    }
    case 0x31U: // get CD device connection
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = 0U;
        command_[2] = static_cast<std::uint16_t>(cdDeviceConnection_) << 8U;
        command_[3] = 0U;
        hirq_ |= kHirqCmok;
        break;
    case 0x32U: // get last buffer destination
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = 0U;
        command_[2] = static_cast<std::uint16_t>(lastBufferDestination_) << 8U;
        command_[3] = 0U;
        hirq_ |= kHirqCmok;
        break;
    case 0x48U: { // reset selector/filter state
        transfer_.clear();
        fifoOffset_ = 0U;
        transferWordsRead_ = 0U;
        transferActive_ = false;
        const auto flags = static_cast<std::uint8_t>(command_[0]);
        if (flags == 0U) {
            // With no reset flags, 0x48 targets one partition (CR3 high
            // byte).  Sega's GFS uses this form after consuming a sector;
            // clearing every selector/filter here makes the next PLAY lose
            // its filter graph and look like a CD-selection reset.
            const auto partition = static_cast<std::uint8_t>(command_[2] >> 8U);
            if (partition < partitionSectorCounts_.size()) {
                partitionSectorCounts_[partition] = 0U;
                partitionStartLba_[partition] = 0U;
                if (streamingPartition_ == partition) {
                    streamingPartition_ = 0xFFU;
                    streamingFilter_ = 0xFFU;
                    streamingCursor_ = 0U;
                    streamingSectorCount_ = 0U;
                }
            }
        } else {
            if ((flags & 0x80U) != 0U) {
                for (auto& filter : filters_) filter.falseConnection = 0xFFU;
            }
            if ((flags & 0x40U) != 0U) {
                for (std::size_t index = 0U; index < filters_.size(); ++index) {
                    filters_[index].trueConnection = static_cast<std::uint8_t>(index);
                }
            }
            if ((flags & 0x10U) != 0U) {
                for (auto& filter : filters_) {
                    const auto trueConnection = filter.trueConnection;
                    const auto falseConnection = filter.falseConnection;
                    filter = FilterState{};
                    filter.trueConnection = trueConnection;
                    filter.falseConnection = falseConnection;
                }
            }
            if ((flags & 0x04U) != 0U) {
                partitionSectorCounts_.fill(0U);
                partitionStartLba_.fill(0U);
                streamingPartition_ = 0xFFU;
                streamingFilter_ = 0xFFU;
                streamingCursor_ = 0U;
                streamingSectorCount_ = 0U;
            }
        }
        directoryFilter_ = 0U;
        directoryFileId_ = 0U;
        calculatedTransferWords_ = 0U;
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = 0U;
        command_[2] = 0U;
        command_[3] = 0U;
        hirq_ &= static_cast<std::uint16_t>(~(kHirqCsct | kHirqDrdy));
        hirq_ |= static_cast<std::uint16_t>(kHirqCmok | kHirqEsel);
        break;
    }
    case 0x50U: // get buffer size
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = freeBufferSectors();
        command_[2] = 0x1800U;
        command_[3] = 200U;
        hirq_ |= kHirqCmok;
        break;
    case 0x51U: { // get number of sectors retained in a partition
        const auto partition = static_cast<std::uint8_t>(command_[2] >> 8U);
        if (partition >= partitionSectorCounts_.size()) {
            command_[0] = 0xFF00U;
            hirq_ |= kHirqCmok;
            break;
        }
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = 0U;
        command_[2] = 0U;
        command_[3] = partitionSectorCounts_[partition];
        stats_.lastPartition = partition;
        stats_.lastSectorNumberPartition = partition;
        stats_.lastPartitionSectorCount = partitionSectorCounts_[partition];
        stats_.lastStreamingCursor = streamingCursor_;
        stats_.lastStreamingSectorCount = streamingSectorCount_;
        ++stats_.sectorNumberQueries;
        stats_.lastSectorNumberResponse = {command_[0], command_[1], command_[2], command_[3]};
        // CDC_GetSectorNumber is consumed through the same command/data
        // handshake as the reference path.  Keep DRDY latched until the
        // caller acknowledges the response; GFS uses this edge to continue
        // draining the retained partition while PLAY is active.
        hirq_ |= static_cast<std::uint16_t>(kHirqCmok | kHirqDrdy);
        stats_.lastSectorNumberHirq = hirq_;
        break;
    }
    case 0x52U: { // calculate actual host-transfer size
        ++stats_.calculateCommands;
        for (std::size_t index = 0U; index < 4U; ++index) {
            stats_.lastCalculateRegisters[index] = command_[index];
        }
        auto offset = static_cast<std::uint32_t>(command_[1]);
        const auto partition = static_cast<std::uint8_t>(command_[2] >> 8U);
        auto count = static_cast<std::uint32_t>(command_[3]);
        if (partition >= partitionSectorCounts_.size()) {
            command_[0] = 0xFF00U;
            hirq_ |= kHirqCmok;
            break;
        }
        const auto retained = static_cast<std::uint32_t>(
            partitionSectorCounts_[partition]);
        if (offset == 0xFFFFU) offset = retained == 0U ? 0U : retained - 1U;
        if (count == 0xFFFFU) count = retained > offset ? retained - offset : 0U;
        // The calculation and the subsequent GET must describe the same
        // complete sector range. A partial size followed by a full GET/DELETE
        // makes the guest discard sectors that it has not copied. Keep the
        // previous result until the requested range is retained (as in the
        // CD Block's WAIT response, also modeled by Mednafen).
        if (count == 0U || offset >= retained || count > retained - offset) {
            ++stats_.calculateWaits;
            command_[0] = static_cast<std::uint16_t>((status_ | 0x80U) << 8U);
            stats_.lastCalculateResult = command_[0];
            hirq_ |= kHirqCmok;
            break;
        }
        constexpr std::array<std::uint32_t, 4> kWordsPerSector{
            1024U, 1168U, 1170U, 1176U};
        calculatedTransferWords_ = count * kWordsPerSector[getSectorLength_];
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = 0x4101U;
        command_[2] = static_cast<std::uint16_t>(
            0x0100U | (currentFad_ >> 16U));
        command_[3] = static_cast<std::uint16_t>(currentFad_);
        stats_.lastCalculateResult = command_[0];
        hirq_ |= static_cast<std::uint16_t>(kHirqCmok | kHirqEsel);
        break;
    }
    case 0x53U: // get previously calculated actual size
        command_[0] = static_cast<std::uint16_t>(
            (status_ << 8U) | ((calculatedTransferWords_ >> 16U) & 0x00FFU));
        command_[1] = static_cast<std::uint16_t>(calculatedTransferWords_);
        command_[2] = 0U;
        command_[3] = 0U;
        hirq_ |= static_cast<std::uint16_t>(kHirqCmok | kHirqEsel);
        break;
    case 0x40U: { // set filter FAD range
        const auto filter = static_cast<std::uint8_t>(command_[2] >> 8U);
        if (filter >= filters_.size()) {
            command_[0] = 0xFF00U;
            hirq_ |= kHirqCmok;
            break;
        }
        filters_[filter].fad =
            (static_cast<std::uint32_t>(command_[0] & 0x00FFU) << 16U) |
            command_[1];
        filters_[filter].range =
            (static_cast<std::uint32_t>(command_[2] & 0x00FFU) << 16U) |
            command_[3];
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = command_[2] = command_[3] = 0U;
        hirq_ |= static_cast<std::uint16_t>(kHirqCmok | kHirqEsel);
        break;
    }
    case 0x41U: { // get filter FAD range
        const auto filter = static_cast<std::uint8_t>(command_[2] >> 8U);
        if (filter >= filters_.size()) {
            command_[0] = 0xFF00U;
            hirq_ |= kHirqCmok;
            break;
        }
        const auto& state = filters_[filter];
        command_[0] = static_cast<std::uint16_t>(
            (status_ << 8U) | ((state.fad >> 16U) & 0xFFU));
        command_[1] = static_cast<std::uint16_t>(state.fad);
        command_[2] = static_cast<std::uint16_t>((state.range >> 16U) & 0xFFU);
        command_[3] = static_cast<std::uint16_t>(state.range);
        hirq_ |= kHirqCmok;
        break;
    }
    case 0x42U: { // set filter subheader conditions
        const auto filter = static_cast<std::uint8_t>(command_[2] >> 8U);
        if (filter >= filters_.size()) {
            command_[0] = 0xFF00U;
            hirq_ |= kHirqCmok;
            break;
        }
        auto& state = filters_[filter];
        state.channel = static_cast<std::uint8_t>(command_[0]);
        state.submodeMask = static_cast<std::uint8_t>(command_[1] >> 8U);
        state.codingMask = static_cast<std::uint8_t>(command_[1]);
        state.file = static_cast<std::uint8_t>(command_[2]);
        state.submode = static_cast<std::uint8_t>(command_[3] >> 8U);
        state.coding = static_cast<std::uint8_t>(command_[3]);
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = 0U;
        command_[2] = 0U;
        command_[3] = 0U;
        hirq_ |= static_cast<std::uint16_t>(kHirqCmok | kHirqEsel);
        break;
    }
    case 0x44U: { // set filter mode
        const auto filter = static_cast<std::uint8_t>(command_[2] >> 8U);
        if (filter >= filters_.size()) {
            command_[0] = 0xFF00U;
            hirq_ |= kHirqCmok;
            break;
        }
        auto& state = filters_[filter];
        state.mode = static_cast<std::uint8_t>(command_[0]);
        if ((state.mode & 0x80U) != 0U) {
            const auto trueConnection = state.trueConnection;
            const auto falseConnection = state.falseConnection;
            state = FilterState{};
            state.trueConnection = trueConnection;
            state.falseConnection = falseConnection;
        }
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = 0U;
        command_[2] = 0U;
        command_[3] = 0U;
        hirq_ |= static_cast<std::uint16_t>(kHirqCmok | kHirqEsel);
        break;
    }
    case 0x43U: { // get filter subheader conditions
        const auto filter = static_cast<std::uint8_t>(command_[2] >> 8U);
        if (filter >= filters_.size()) {
            command_[0] = 0xFF00U;
            hirq_ |= kHirqCmok;
            break;
        }
        const auto& state = filters_[filter];
        command_[0] = static_cast<std::uint16_t>((status_ << 8U) | state.channel);
        command_[1] = static_cast<std::uint16_t>(state.submodeMask << 8U) |
                      state.codingMask;
        command_[2] = state.file;
        command_[3] = static_cast<std::uint16_t>(state.submode << 8U) |
                      state.coding;
        hirq_ |= static_cast<std::uint16_t>(kHirqCmok | kHirqEsel);
        break;
    }
    case 0x45U: { // get filter mode
        const auto filter = static_cast<std::uint8_t>(command_[2] >> 8U);
        if (filter >= filters_.size()) {
            command_[0] = 0xFF00U;
            hirq_ |= kHirqCmok;
            break;
        }
        command_[0] = static_cast<std::uint16_t>((status_ << 8U) |
                                                 filters_[filter].mode);
        command_[1] = command_[2] = command_[3] = 0U;
        hirq_ |= static_cast<std::uint16_t>(kHirqCmok | kHirqEsel);
        break;
    }
    case 0x46U: { // set filter true/false connections
        const auto flags = static_cast<std::uint8_t>(command_[0]);
        const auto trueConnection = static_cast<std::uint8_t>(command_[1] >> 8U);
        const auto falseConnection = static_cast<std::uint8_t>(command_[1]);
        const auto filter = static_cast<std::uint8_t>(command_[2] >> 8U);
        const auto validConnection = [this](std::uint8_t connection) {
            return connection == 0xFFU || connection < filters_.size();
        };
        if (filter >= filters_.size() ||
            ((flags & 1U) != 0U && !validConnection(trueConnection)) ||
            ((flags & 2U) != 0U && !validConnection(falseConnection))) {
            command_[0] = 0xFF00U;
            hirq_ |= kHirqCmok;
            break;
        }
        if ((flags & 1U) != 0U) filters_[filter].trueConnection = trueConnection;
        if ((flags & 2U) != 0U) filters_[filter].falseConnection = falseConnection;
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = 0U;
        command_[2] = 0U;
        command_[3] = 0U;
        hirq_ |= static_cast<std::uint16_t>(kHirqCmok | kHirqEsel);
        break;
    }
    case 0x47U: { // get filter connections
        const auto filter = static_cast<std::uint8_t>(command_[2] >> 8U);
        if (filter >= filters_.size()) {
            command_[0] = 0xFF00U;
            hirq_ |= kHirqCmok;
            break;
        }
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = static_cast<std::uint16_t>(
            filters_[filter].trueConnection << 8U) |
            filters_[filter].falseConnection;
        command_[2] = command_[3] = 0U;
        hirq_ |= kHirqCmok;
        break;
    }
    case 0x54U: { // get retained sector information
        const auto sector = static_cast<std::uint32_t>(command_[1]);
        const auto partition = static_cast<std::uint8_t>(command_[2] >> 8U);
        if (partition >= partitionSectorCounts_.size() ||
            sector >= partitionSectorCounts_[partition]) {
            command_[0] = static_cast<std::uint16_t>(0xFF00U |
                                                     (command_[0] & 0x00FFU));
            hirq_ |= static_cast<std::uint16_t>(kHirqCmok | kHirqEsel);
            break;
        }
        const auto fad = partitionStartLba_[partition] + sector + 150U;
        command_[0] = static_cast<std::uint16_t>(
            (status_ << 8U) | ((fad >> 16U) & 0xFFU));
        command_[1] = static_cast<std::uint16_t>(fad);
        command_[2] = 0U; // file/channel subheader for Mode-1 data
        command_[3] = 0U; // submode/coding information
        hirq_ |= static_cast<std::uint16_t>(kHirqCmok | kHirqEsel);
        break;
    }
    case 0x60U: { // set get/put sector length
        const auto getLength = static_cast<std::uint8_t>(command_[0]);
        const auto putLength = static_cast<std::uint8_t>(command_[1] >> 8U);
        const auto invalid = [](std::uint8_t length) {
            return length != 0xFFU && length > 3U;
        };
        if (invalid(getLength) || invalid(putLength)) {
            command_[0] = 0xFF00U;
            hirq_ |= kHirqCmok;
            break;
        }
        if (getLength != 0xFFU) getSectorLength_ = getLength;
        if (putLength != 0xFFU) putSectorLength_ = putLength;
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = 0U;
        command_[2] = 0U;
        command_[3] = 0U;
        hirq_ |= static_cast<std::uint16_t>(kHirqCmok | kHirqEsel);
        break;
    }
    case 0x61U: // get sector data from a retained partition
    case 0x62U: // delete sector data from a retained partition
    case 0x63U: { // get and delete sector data from a retained partition
        auto offset = static_cast<std::uint32_t>(command_[1]);
        const auto partition = static_cast<std::uint8_t>(command_[2] >> 8U);
        auto count = static_cast<std::uint32_t>(command_[3]);
        if (partition >= partitionSectorCounts_.size()) {
            command_[0] = 0xFF00U;
            hirq_ |= kHirqCmok;
            break;
        }
        const auto retained = static_cast<std::uint32_t>(
            partitionSectorCounts_[partition]);
        stats_.lastDataOffset = offset;
        stats_.lastDataCount = count;
        stats_.lastDataRetained = static_cast<std::uint16_t>(retained);
        stats_.lastDataInstruction = instruction;
        if (instruction == 0x62U || instruction == 0x63U) {
            ++stats_.deleteCommands;
            stats_.lastDeletePartition = partition;
            stats_.lastDeleteOffset = offset;
            stats_.lastDeleteCount = count;
            stats_.lastDeleteBefore = static_cast<std::uint16_t>(retained);
        }
        if (offset == 0xFFFFU) offset = retained == 0U ? 0U : retained - 1U;
        if (count == 0xFFFFU) count = retained > offset ? retained - offset : 0U;
        if (count == 0U || offset >= retained || count > retained - offset) {
            if (instruction == 0x61U || instruction == 0x63U) {
                ++stats_.transferWaits;
                stats_.lastTransferWaitOffset = offset;
                stats_.lastTransferWaitCount = count;
                stats_.lastTransferWaitRetained = static_cast<std::uint16_t>(retained);
            }
            if (instruction == 0x62U || instruction == 0x63U) ++stats_.deleteWaits;
            command_[0] = 0x8200U; // WAIT: requested partition range is not ready
            hirq_ |= kHirqCmok;
            break;
        }

        const auto returnsData = instruction != 0x62U;
        const auto deletesData = instruction != 0x61U;
        if (instruction == 0x61U || instruction == 0x63U) {
            stats_.transferSectorRequests += count;
        }
        if (instruction == 0x62U || instruction == 0x63U) {
            stats_.deleteSectorRequests += count;
        }
        if (returnsData) {
            stats_.lastTransferLba = partitionStartLba_[partition] + offset;
            stats_.lastTransferOffset = offset;
            stats_.lastTransferCount = count;
            stats_.lastTransferPartition = partition;
            stats_.lastTransferInstruction = instruction;
            transfer_.clear();
            const auto bytesPerSector = encodeSectorForTransfer(
                partitionStartLba_[partition] + offset);
            transfer_.reserve(bytesPerSector.size() * count);
            for (std::uint32_t index = 0U; index < count; ++index) {
                auto sector = encodeSectorForTransfer(
                    partitionStartLba_[partition] + offset + index);
                if (sector.empty()) {
                    transfer_.clear();
                    break;
                }
                transfer_.insert(transfer_.end(), sector.begin(), sector.end());
            }
            fifoOffset_ = 0U;
            transferWordsRead_ = 0U;
            transferActive_ = !transfer_.empty();
            if (!transferActive_) {
                command_[0] = 0xFF00U;
                hirq_ |= kHirqCmok;
                break;
            }
        }

        if (deletesData) {
            // Rockman consumes from the head. Preserve a contiguous model for
            // tail deletion too; a middle deletion retains the prefix because
            // this deterministic runtime stores one extent per partition.
            if (offset == 0U) {
                partitionStartLba_[partition] += count;
                partitionSectorCounts_[partition] = static_cast<std::uint16_t>(
                    retained - count);
            } else {
                // With one contiguous extent, removing a tail is exact. For
                // a middle range, retain the prefix and discard the now
                // discontiguous suffix.
                partitionSectorCounts_[partition] = static_cast<std::uint16_t>(
                    offset);
            }
            const auto anyRetained = std::any_of(
                partitionSectorCounts_.begin(), partitionSectorCounts_.end(),
                [](std::uint16_t sectors) { return sectors != 0U; });
            if (!anyRetained) {
                // CSCT is the level indication that at least one filtered
                // sector is still buffered.  A completed 0x62/0x63 delete
                // must drop it, otherwise GFS re-enters its selector-recovery
                // path while believing stale data is available.
                hirq_ &= static_cast<std::uint16_t>(~kHirqCsct);
            }
            if (instruction == 0x62U || instruction == 0x63U) {
                stats_.lastDeleteAfter = partitionSectorCounts_[partition];
            }
        }

        // The transfer qualifier is not part of CR1.  Yabause's reference
        // path reports the ordinary drive status here; TRNS is a status
        // value used by other command paths, not a flag added to GET DATA.
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = 0x4101U;
        command_[2] = static_cast<std::uint16_t>(0x0100U | (currentFad_ >> 16U));
        command_[3] = static_cast<std::uint16_t>(currentFad_);
        hirq_ |= static_cast<std::uint16_t>(
            kHirqCmok | (returnsData ? kHirqDrdy : 0U) |
            kHirqEhst);
        break;
    }
    case 0x70U: // change directory; root is already retained by this HLE
    case 0x71U: { // read directory; complete the retained root table now
        const auto filter = static_cast<std::uint8_t>(command_[2] >> 8U);
        const auto fileId = (static_cast<std::uint32_t>(command_[2] & 0x00FFU) << 16U) |
                            command_[3];
        // The real block rejects an invalid filter.  A zero file ID denotes
        // the ISO root in the BIOS path; accepting the first retained entry
        // as well keeps callers that use the Get File Scope start ID valid.
        const auto validFilter = filter == 0xFFU || filter < filters_.size();
        const auto validFile = fileId == 0U || fileId == 2U || fileId == 0xFFFFFFU;
        if (!hasImage() || !validFilter || !validFile) {
            command_[0] = 0xFF00U;
            command_[1] = command_[2] = command_[3] = 0U;
            hirq_ |= kHirqCmok;
            break;
        }
        directoryFilter_ = filter == 0xFFU ? 0U : filter;
        directoryFileId_ = fileId;
        status_ = 2U;
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = 0U;
        command_[2] = 0U;
        command_[3] = 0U;
        hirq_ |= static_cast<std::uint16_t>(kHirqCmok |
                                           (hasImage() ? kHirqEfls : 0U));
        break;
    }
    case 0x72U: { // get retained file-system scope for the root directory
        const auto count = std::min<std::size_t>(rootFileCount(), 254U);
        command_[0] = static_cast<std::uint16_t>((hasImage() ? 2U : 7U) << 8U);
        command_[1] = static_cast<std::uint16_t>(count);
        // The root table is fully available synchronously: bit 8 reports the
        // final directory record and the first retained file identifier is 2.
        command_[2] = 0x0100U;
        command_[3] = 2U;
        hirq_ |= kHirqCmok;
        break;
    }
    case 0x73U: { // get retained file information through the host FIFO
        const auto fileId = (static_cast<std::uint32_t>(command_[2] & 0x00FFU) << 16U) |
                            command_[3];
        transfer_ = encodeFileInfo(fileId);
        fifoOffset_ = 0U;
        transferWordsRead_ = 0U;
        transferActive_ = !transfer_.empty();
        if (!transferActive_) {
            command_[0] = 0xFF00U;
            command_[1] = 0U;
            command_[2] = 0U;
            command_[3] = 0U;
            hirq_ |= kHirqCmok;
            break;
        }
        status_ = hasImage() ? 2U : 7U;
        // File-info transfers use the ordinary status in CR1; the transfer
        // is indicated by DRDY, not by OR-ing the TRNS value into status.
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = static_cast<std::uint16_t>(transfer_.size() / 2U);
        command_[2] = 0U;
        command_[3] = 0U;
        hirq_ |= static_cast<std::uint16_t>(kHirqCmok | kHirqDrdy);
        break;
    }
    case 0x74U: { // read file by retained file id (filesystem data stream)
        const auto offset = (static_cast<std::uint32_t>(command_[0] & 0x00FFU) << 8U) |
                            command_[1];
        const auto filter = static_cast<std::uint8_t>(command_[2] >> 8U);
        const auto fileId = (static_cast<std::uint32_t>(command_[2] & 0x00FFU) << 8U) |
                            command_[3];
        if (filter != 0xFFU && filter >= filters_.size()) {
            command_[0] = 0xFF00U;
            command_[1] = command_[2] = command_[3] = 0U;
            hirq_ |= kHirqCmok;
            break;
        }
        const auto entries = rootFileInfo();
        if (fileId < 2U || fileId - 2U >= entries.size()) {
            command_[0] = 0xFF00U;
            hirq_ |= kHirqCmok;
            break;
        }
        const auto& entry = entries[fileId - 2U];
        const auto fileLba = entry.fad >= 150U ? entry.fad - 150U : 0U;
        if (offset >= entry.size) {
            transfer_.clear();
        } else {
            const auto available = static_cast<std::size_t>(entry.size - offset);
            const auto bytes = readBytes(static_cast<std::uint64_t>(fileLba) *
                                             kLogicalSectorSize + offset, available);
            transfer_ = bytes.value_or(std::vector<std::byte>{});
        }
        fifoOffset_ = 0U;
        transferWordsRead_ = 0U;
        transferActive_ = !transfer_.empty();
        if (transferActive_ && filter != 0xFFU) {
            lastBufferDestination_ = filter;
        }
        status_ = hasImage() ? 2U : 7U;
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = static_cast<std::uint16_t>(
            (transfer_.size() + 1U) / 2U);
        command_[2] = 0U;
        command_[3] = 0U;
        hirq_ |= static_cast<std::uint16_t>(kHirqCmok |
                                           (transferActive_ ? kHirqDrdy : kHirqEfls));
        break;
    }
    case 0x75U: // abort the active filesystem operation
        transfer_.clear();
        fifoOffset_ = 0U;
        transferWordsRead_ = 0U;
        transferActive_ = false;
        status_ = hasImage() ? 1U : 7U;
        command_[0] = static_cast<std::uint16_t>(status_ << 8U);
        command_[1] = 0U;
        command_[2] = 0x0100U;
        command_[3] = static_cast<std::uint16_t>(currentFad_);
        hirq_ &= static_cast<std::uint16_t>(~(kHirqCsct | kHirqDrdy));
        hirq_ |= static_cast<std::uint16_t>(kHirqCmok |
                                           (hasImage() ? kHirqEfls : 0U));
        break;
    default:
        command_[0] = 0xFF00U;
        hirq_ |= kHirqCmok;
        break;
    }
    stats_.lastStatus = status_;
}

} // namespace saturn::platform
