#include "MainFrame.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <exception>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <utility>
#include <map>
#include <mutex>
#include <thread>
#include <cstdint>

#include <wx/dirdlg.h>
#include <wx/artprov.h>
#include <wx/filedlg.h>
#include <wx/filefn.h>
#include <wx/utils.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>
#include <wx/stdpaths.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <zlib.h>

#ifdef _WIN32
#include <commctrl.h>
#endif

wxDEFINE_EVENT(wxEVT_PZMM_JOB_DONE, wxThreadEvent);
wxDEFINE_EVENT(wxEVT_PZMM_PROGRESS, wxThreadEvent);

namespace {

constexpr const char* kModArchiveMarkerPath = "__PZModManagerArchive.txt";
constexpr const char* kModArchiveMarkerText = "PZModManager mod archive v1\n";

std::string ToUtf8(const wxString& value);
wxString PathToWx(const pzmm::fs::path& path);
std::string LowerFilterText(std::string value);
std::vector<std::string> NumericVersionParts(const std::string& value);

wxString FromUtf8(const std::string& value) {
    return wxString::FromUTF8(value.c_str());
}

std::optional<int> CompareImportModVersions(const std::string& imported, const std::string& installed) {
    const std::string left = LowerFilterText(imported);
    const std::string right = LowerFilterText(installed);
    if (left.empty() || right.empty()) return std::nullopt;
    if (left == right) return 0;
    const auto leftParts = NumericVersionParts(left);
    const auto rightParts = NumericVersionParts(right);
    if (leftParts.empty() || rightParts.empty()) return std::nullopt;

    const size_t count = std::max(leftParts.size(), rightParts.size());
    for (size_t i = 0; i < count; ++i) {
        std::string a = i < leftParts.size() ? leftParts[i] : "0";
        std::string b = i < rightParts.size() ? rightParts[i] : "0";
        const auto aFirst = a.find_first_not_of('0');
        const auto bFirst = b.find_first_not_of('0');
        a = aFirst == std::string::npos ? "0" : a.substr(aFirst);
        b = bFirst == std::string::npos ? "0" : b.substr(bFirst);
        if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
        if (a != b) return a < b ? -1 : 1;
    }
    return 0;
}

struct ArchiveInspection {
    bool marked = false;
    std::map<std::string, std::string> modRoots;
    std::string error;
};

bool NormalizeZipEntryName(std::string& name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty() || name.front() == '/' || name.find(':') != std::string::npos) return false;
    size_t start = 0;
    while (start <= name.size()) {
        const size_t end = name.find('/', start);
        const std::string component = name.substr(start, end == std::string::npos ? end : end - start);
        if (component == ".." || component == ".") return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    while (!name.empty() && name.back() == '/') name.pop_back();
    return !name.empty();
}

bool LooksLikeModInfoPath(const std::string& relativePath) {
    const std::string lower = LowerFilterText(relativePath);
    return lower == "mod.info" || (lower.size() >= 9 && lower.compare(lower.size() - 9, 9, "/mod.info") == 0);
}

std::string ModRootForInfoEntry(const std::string& name) {
    std::vector<std::string> components;
    size_t start = 0;
    while (start <= name.size()) {
        const size_t end = name.find('/', start);
        components.push_back(name.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (components.empty() || LowerFilterText(components.back()) != "mod.info") return {};
    components.pop_back();
    while (components.size() > 1) {
        const auto& last = components.back();
        const bool versionFolder = !last.empty() &&
            std::all_of(last.begin(), last.end(), [](unsigned char ch) { return std::isdigit(ch) || ch == '.'; }) &&
            std::any_of(last.begin(), last.end(), [](unsigned char ch) { return std::isdigit(ch); });
        if (LowerFilterText(last) == "common" || versionFolder) components.pop_back();
        else break;
    }
    if (components.empty()) return "_root";
    std::string root = components.front();
    for (size_t i = 1; i < components.size(); ++i) root += "/" + components[i];
    return root;
}

ArchiveInspection InspectModArchive(const pzmm::fs::path& archivePath) {
    ArchiveInspection inspection;
    wxFileInputStream input(PathToWx(archivePath));
    if (!input.IsOk()) {
        inspection.error = "Could not open the selected ZIP archive.";
        return inspection;
    }
    wxZipInputStream zip(input);
    std::string markerText;
    std::set<std::string> candidateRoots;
    std::unique_ptr<wxZipEntry> entry;
    while ((entry.reset(zip.GetNextEntry()), entry)) {
        std::string name = ToUtf8(entry->GetName(wxPATH_UNIX));
        if (!NormalizeZipEntryName(name)) continue;
        if (name == kModArchiveMarkerPath) {
            char marker[128]{};
            zip.Read(marker, sizeof(marker));
            markerText.assign(marker, zip.LastRead());
            continue;
        }

        const size_t slash = name.find('/');
        const std::string root = slash == std::string::npos ? "_root" : name.substr(0, slash);
        candidateRoots.insert(root);
    }
    inspection.marked = markerText == kModArchiveMarkerText;
    if (!inspection.marked) {
        std::set<std::string> metadataRoots;
        wxFileInputStream secondInput(PathToWx(archivePath));
        wxZipInputStream secondZip(secondInput);
        while ((entry.reset(secondZip.GetNextEntry()), entry)) {
            std::string name = ToUtf8(entry->GetName(wxPATH_UNIX));
            if (!NormalizeZipEntryName(name) || entry->IsDir()) continue;
            if (name != kModArchiveMarkerPath && LooksLikeModInfoPath(name))
                metadataRoots.insert(ModRootForInfoEntry(name));
        }
        candidateRoots = metadataRoots.empty() ? std::move(candidateRoots) : std::move(metadataRoots);
    }
    std::set<std::string> stagedNames;
    for (const auto& root : candidateRoots) {
        const size_t slash = root.find_last_of('/');
        const std::string base = root == "_root" ? "Imported mod" : root.substr(slash == std::string::npos ? 0 : slash + 1);
        std::string staged = base;
        for (size_t suffix = 2; !stagedNames.insert(LowerFilterText(staged)).second; ++suffix)
            staged = base + " (" + std::to_string(suffix) + ")";
        inspection.modRoots.emplace(root, staged);
    }
    if (inspection.modRoots.empty()) inspection.error = "No mod folders could be identified in this archive.";
    return inspection;
}

bool ExtractModArchive(const pzmm::fs::path& archivePath,
                       const pzmm::fs::path& destinationRoot,
                       const ArchiveInspection& inspection,
                       const std::atomic<bool>& cancel,
                       std::string& error) {
    std::error_code ec;
    pzmm::fs::create_directories(destinationRoot, ec);
    if (ec) {
        error = "Could not create temporary import folder: " + ec.message();
        return false;
    }
    wxFileInputStream input(PathToWx(archivePath));
    if (!input.IsOk()) {
        error = "Could not reopen the selected ZIP archive.";
        return false;
    }
    wxZipInputStream zip(input);
    std::unique_ptr<wxZipEntry> entry;
    char buffer[64 * 1024];
    while ((entry.reset(zip.GetNextEntry()), entry)) {
        if (cancel.load()) {
            error = "Archive import was cancelled.";
            return false;
        }
        std::string name = ToUtf8(entry->GetName(wxPATH_UNIX));
        if (!NormalizeZipEntryName(name) || name == kModArchiveMarkerPath || entry->IsDir()) continue;
        auto matchedRoot = inspection.modRoots.end();
        for (auto root = inspection.modRoots.begin(); root != inspection.modRoots.end(); ++root) {
            const bool matches = root->first == "_root"
                ? (inspection.modRoots.size() == 1 || name.find('/') == std::string::npos)
                : name.compare(0, root->first.size(), root->first) == 0 &&
                  name.size() > root->first.size() && name[root->first.size()] == '/';
            if (matches && (matchedRoot == inspection.modRoots.end() || root->first.size() > matchedRoot->first.size()))
                matchedRoot = root;
        }
        if (matchedRoot == inspection.modRoots.end()) continue;
        const std::string relative = matchedRoot->first == "_root"
            ? name
            : name.substr(matchedRoot->first.size() + 1);
        const auto outputPath = destinationRoot / pzmm::fs::u8path(matchedRoot->second) /
                                pzmm::fs::u8path(relative);
        pzmm::fs::create_directories(outputPath.parent_path(), ec);
        if (ec) {
            error = "Could not create an extracted mod folder: " + ec.message();
            return false;
        }
        std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Could not write extracted file: " + pzmm::PathToUtf8(outputPath);
            return false;
        }
        while (!zip.Eof() && !cancel.load()) {
            zip.Read(buffer, sizeof(buffer));
            const size_t count = zip.LastRead();
            if (count == 0) break;
            output.write(buffer, static_cast<std::streamsize>(count));
            if (!output) {
                error = "Could not write extracted file: " + pzmm::PathToUtf8(outputPath);
                return false;
            }
        }
        if (cancel.load()) {
            error = "Archive import was cancelled.";
            return false;
        }
        if (!zip.IsOk()) {
            error = "A ZIP entry could not be read: " + name;
            return false;
        }
    }
    return true;
}

wxString FolderModifiedLabel(const pzmm::ModEntry& mod) {
    if (mod.folderModifiedTime == pzmm::fs::file_time_type::min()) return "Unknown";
    const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        mod.folderModifiedTime - pzmm::fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    const wxDateTime date(std::chrono::system_clock::to_time_t(systemTime));
    return date.Format("%Y-%m-%d %H:%M");
}

std::string ToUtf8(const wxString& value) {
    const auto buffer = value.ToUTF8();
    return buffer ? std::string(buffer.data(), buffer.length()) : std::string{};
}

pzmm::fs::path ToFsPath(const wxString& value) {
#if defined(_WIN32)
    return pzmm::fs::path(value.ToStdWstring());
#else
    return pzmm::fs::u8path(ToUtf8(value));
#endif
}

pzmm::fs::path MetadataCacheDirectory() {
    return ToFsPath(wxStandardPaths::Get().GetUserLocalDataDir()) / "metadata-cache";
}

wxString PathToWx(const pzmm::fs::path& path) {
#if defined(_WIN32)
    return wxString(path.wstring());
#else
    return FromUtf8(pzmm::PathToUtf8(path));
#endif
}

wxString DefaultUserModsPath() {
    return wxGetHomeDir() + wxFILE_SEP_PATH + "Zomboid" + wxFILE_SEP_PATH + "mods";
}

wxString DefaultWorkshopPath() {
    wxString programFiles;
    if (!wxGetEnv("ProgramFiles(x86)", &programFiles) || programFiles.empty()) {
        programFiles = "C:\\Program Files (x86)";
    }
    return programFiles + wxFILE_SEP_PATH + "Steam" + wxFILE_SEP_PATH +
           "steamapps" + wxFILE_SEP_PATH + "workshop" + wxFILE_SEP_PATH +
           "content" + wxFILE_SEP_PATH + "108600";
}

std::string VersionsLabel(const pzmm::ModEntry& entry) {
    if (entry.gameVersions.empty()) return entry.versions.empty() ? "No mod.info" : "root mod.info";
    return pzmm::Join(entry.gameVersions, ", ");
}

std::string EntryLoadStatus(const pzmm::ModEntry& entry) {
    return pzmm::LoadStatus(entry);
}

std::string LowerFilterText(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::vector<std::string> NumericVersionParts(const std::string& value) {
    std::vector<std::string> parts;
    std::string part;
    for (const unsigned char ch : value) {
        if (std::isdigit(ch)) {
            part.push_back(static_cast<char>(ch));
        } else if (!part.empty()) {
            parts.push_back(std::move(part));
            part.clear();
        }
    }
    if (!part.empty()) parts.push_back(std::move(part));
    return parts;
}

bool VersionMatchesBucket(const std::string& version, const std::string& bucket) {
    const std::string normalized = LowerFilterText(version);
    if (normalized.compare(0, bucket.size(), bucket) != 0) return false;
    return normalized.size() == bucket.size() || normalized[bucket.size()] == '.';
}

bool OpenInExplorer(const pzmm::fs::path& path, bool selectFile = false) {
#if defined(_WIN32)
    wxString command = "explorer.exe ";
    if (selectFile) command += "/select,";
    command += "\"" + PathToWx(path) + "\"";
    return wxExecute(command, wxEXEC_ASYNC) != 0;
#else
    const auto target = selectFile ? path.parent_path() : path;
    return wxLaunchDefaultApplication(PathToWx(target));
#endif
}

struct ArchiveFile {
    pzmm::fs::path path;
    std::string entryName;
};

struct CompressedArchiveFile {
    ArchiveFile file;
    pzmm::fs::path compressedPath;
    uint64_t uncompressedSize = 0;
    uint64_t compressedSize = 0;
    uint32_t crc = 0;
    std::string error;
};

class CompressionScheduler {
public:
    explicit CompressionScheduler(size_t maximumWorkers) : m_maximumWorkers(maximumWorkers), m_workerLimit(maximumWorkers) {}

    bool Acquire(const std::atomic<bool>& cancel, const std::atomic<bool>& stop) {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (!cancel.load() && !stop.load()) {
            UpdateWorkerLimit();
            if (m_activeWorkers < m_workerLimit) {
                ++m_activeWorkers;
                return true;
            }
            m_condition.wait_for(lock, std::chrono::milliseconds(100));
        }
        return false;
    }

    void Release() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_activeWorkers > 0) --m_activeWorkers;
        }
        m_condition.notify_all();
    }

    bool Checkpoint(const std::atomic<bool>& cancel, const std::atomic<bool>& stop) {
        std::unique_lock<std::mutex> lock(m_mutex);
        bool releasedSlot = false;
        while (!cancel.load() && !stop.load()) {
            UpdateWorkerLimit();
            if (!releasedSlot && m_activeWorkers > m_workerLimit) {
                --m_activeWorkers;
                releasedSlot = true;
                m_condition.notify_all();
            }
            if (releasedSlot && m_activeWorkers < m_workerLimit) {
                ++m_activeWorkers;
                return true;
            }
            if (!releasedSlot) return true;
            m_condition.wait_for(lock, std::chrono::milliseconds(100));
        }
        if (releasedSlot) ++m_activeWorkers;
        return false;
    }

    void Wake() { m_condition.notify_all(); }

private:
    void UpdateWorkerLimit() {
#ifdef _WIN32
        const auto now = std::chrono::steady_clock::now();
        if (m_lastSample.time_since_epoch().count() != 0 &&
            now - m_lastSample < std::chrono::milliseconds(500)) return;
        m_lastSample = now;

        FILETIME idleTime{}, kernelTime{}, userTime{}, processCreate{}, processExit{}, processKernel{}, processUser{};
        if (!GetSystemTimes(&idleTime, &kernelTime, &userTime) ||
            !GetProcessTimes(GetCurrentProcess(), &processCreate, &processExit, &processKernel, &processUser)) return;

        const auto value = [](const FILETIME& time) {
            return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        };
        const uint64_t idle = value(idleTime);
        const uint64_t kernel = value(kernelTime);
        const uint64_t user = value(userTime);
        const uint64_t process = value(processKernel) + value(processUser);
        if (m_hasSample) {
            const uint64_t totalDelta = (kernel - m_lastKernel) + (user - m_lastUser);
            const uint64_t idleDelta = idle - m_lastIdle;
            const uint64_t processDelta = process - m_lastProcess;
            if (totalDelta > 0) {
                const uint64_t busyDelta = totalDelta > idleDelta ? totalDelta - idleDelta : 0;
                const uint64_t otherBusy = busyDelta > processDelta ? busyDelta - processDelta : 0;
                const double otherLoad = static_cast<double>(otherBusy) / static_cast<double>(totalDelta);
                if (otherLoad >= 0.75) m_workerLimit = std::max<size_t>(1, m_maximumWorkers / 4);
                else if (otherLoad >= 0.55) m_workerLimit = std::max<size_t>(1, m_maximumWorkers / 2);
                else if (otherLoad >= 0.35) m_workerLimit = std::max<size_t>(1, (m_maximumWorkers * 3) / 4);
                else m_workerLimit = m_maximumWorkers;
            }
        }
        m_lastIdle = idle;
        m_lastKernel = kernel;
        m_lastUser = user;
        m_lastProcess = process;
        m_hasSample = true;
#endif
    }

    const size_t m_maximumWorkers;
    size_t m_workerLimit;
    size_t m_activeWorkers = 0;
    std::mutex m_mutex;
    std::condition_variable m_condition;
#ifdef _WIN32
    bool m_hasSample = false;
    std::chrono::steady_clock::time_point m_lastSample{};
    uint64_t m_lastIdle = 0;
    uint64_t m_lastKernel = 0;
    uint64_t m_lastUser = 0;
    uint64_t m_lastProcess = 0;
#endif
};

bool CompressArchiveFile(const ArchiveFile& file,
                         const pzmm::fs::path& compressedPath,
                         const std::atomic<bool>& cancel,
                         CompressionScheduler* scheduler,
                         const std::atomic<bool>* stop,
                         CompressedArchiveFile& result) {
    result.file = file;
    result.compressedPath = compressedPath;
    std::ifstream input(file.path, std::ios::binary);
    std::ofstream output(compressedPath, std::ios::binary | std::ios::trunc);
    if (!input || !output) {
        result.error = "Could not open " + pzmm::PathToUtf8(file.path) + " for compression.";
        return false;
    }

    z_stream stream{};
    const int initResult = deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    if (initResult != Z_OK) {
        result.error = "Could not initialize Deflate compression.";
        return false;
    }

    constexpr size_t bufferSize = 256 * 1024;
    std::array<Bytef, bufferSize> inputBuffer{};
    std::array<Bytef, bufferSize> outputBuffer{};
    uLong crc = crc32(0L, Z_NULL, 0);
    const auto fail = [&](const std::string& message) {
        result.error = message;
        deflateEnd(&stream);
        output.close();
        std::error_code removeError;
        pzmm::fs::remove(compressedPath, removeError);
        return false;
    };

    while (!cancel.load()) {
        input.read(reinterpret_cast<char*>(inputBuffer.data()), static_cast<std::streamsize>(inputBuffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            const auto inputCount = static_cast<uInt>(count);
            crc = crc32(crc, inputBuffer.data(), inputCount);
            result.uncompressedSize += static_cast<uint64_t>(count);
            stream.next_in = inputBuffer.data();
            stream.avail_in = inputCount;
            do {
                stream.next_out = outputBuffer.data();
                stream.avail_out = static_cast<uInt>(outputBuffer.size());
                const int status = deflate(&stream, Z_NO_FLUSH);
                if (status != Z_OK) return fail("Deflate compression failed for " + pzmm::PathToUtf8(file.path) + ".");
                const size_t produced = outputBuffer.size() - stream.avail_out;
                if (produced != 0) {
                    output.write(reinterpret_cast<const char*>(outputBuffer.data()), static_cast<std::streamsize>(produced));
                    if (!output) return fail("Could not write compressed data for " + pzmm::PathToUtf8(file.path) + ".");
                    result.compressedSize += produced;
                }
            } while (stream.avail_in != 0 || stream.avail_out == 0);
            if (scheduler && stop && !scheduler->Checkpoint(cancel, *stop))
                return fail(cancel.load() ? "Archive creation was cancelled." : "Archive compression was stopped.");
        }
        if (input.bad()) return fail("Could not read " + pzmm::PathToUtf8(file.path) + ".");
        if (input.eof()) break;
    }
    if (cancel.load()) return fail("Archive creation was cancelled.");

    int finishStatus = Z_OK;
    while (finishStatus != Z_STREAM_END) {
        stream.next_out = outputBuffer.data();
        stream.avail_out = static_cast<uInt>(outputBuffer.size());
        finishStatus = deflate(&stream, Z_FINISH);
        if (finishStatus != Z_OK && finishStatus != Z_STREAM_END)
            return fail("Could not finish Deflate compression for " + pzmm::PathToUtf8(file.path) + ".");
        const size_t produced = outputBuffer.size() - stream.avail_out;
        if (produced != 0) {
            output.write(reinterpret_cast<const char*>(outputBuffer.data()), static_cast<std::streamsize>(produced));
            if (!output) return fail("Could not write compressed data for " + pzmm::PathToUtf8(file.path) + ".");
            result.compressedSize += produced;
        }
    }
    deflateEnd(&stream);
    output.close();
    if (!output) {
        result.error = "Could not finalize compressed data for " + pzmm::PathToUtf8(file.path) + ".";
        return false;
    }
    result.crc = static_cast<uint32_t>(crc);
    return true;
}

bool WriteZipU16(std::ostream& output, uint16_t value) {
    const char bytes[] = {static_cast<char>(value), static_cast<char>(value >> 8)};
    output.write(bytes, sizeof(bytes));
    return static_cast<bool>(output);
}

bool WriteZipU32(std::ostream& output, uint32_t value) {
    const char bytes[] = {static_cast<char>(value), static_cast<char>(value >> 8),
                          static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
    output.write(bytes, sizeof(bytes));
    return static_cast<bool>(output);
}

bool WriteZipU64(std::ostream& output, uint64_t value) {
    return WriteZipU32(output, static_cast<uint32_t>(value)) &&
           WriteZipU32(output, static_cast<uint32_t>(value >> 32));
}

struct ZipCentralRecord {
    std::string name;
    uint32_t crc = 0;
    uint64_t compressedSize = 0;
    uint64_t uncompressedSize = 0;
    uint64_t localOffset = 0;
};

bool WriteZipEntry(std::ostream& output, const CompressedArchiveFile& file, ZipCentralRecord& record) {
    if (file.file.entryName.size() > std::numeric_limits<uint16_t>::max()) return false;
    const auto position = output.tellp();
    if (position == std::streampos(-1)) return false;
    record.name = file.file.entryName;
    record.crc = file.crc;
    record.compressedSize = file.compressedSize;
    record.uncompressedSize = file.uncompressedSize;
    record.localOffset = static_cast<uint64_t>(static_cast<std::streamoff>(position));

    if (!WriteZipU32(output, 0x04034b50) || !WriteZipU16(output, 45) || !WriteZipU16(output, 0x0800) ||
        !WriteZipU16(output, 8) || !WriteZipU16(output, 0) || !WriteZipU16(output, 0x0021) ||
        !WriteZipU32(output, record.crc) || !WriteZipU32(output, 0xffffffff) ||
        !WriteZipU32(output, 0xffffffff) || !WriteZipU16(output, static_cast<uint16_t>(record.name.size())) ||
        !WriteZipU16(output, 20)) return false;
    output.write(record.name.data(), static_cast<std::streamsize>(record.name.size()));
    if (!WriteZipU16(output, 1) || !WriteZipU16(output, 16) ||
        !WriteZipU64(output, record.uncompressedSize) || !WriteZipU64(output, record.compressedSize)) return false;

    std::ifstream compressed(file.compressedPath, std::ios::binary);
    if (!compressed) return false;
    std::array<char, 256 * 1024> buffer{};
    while (compressed.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || compressed.gcount() > 0) {
        output.write(buffer.data(), compressed.gcount());
        if (!output) return false;
    }
    return !compressed.bad();
}

bool WriteZipCentralDirectory(std::ostream& output, const std::vector<ZipCentralRecord>& records) {
    const auto directoryStart = output.tellp();
    if (directoryStart == std::streampos(-1)) return false;
    for (const auto& record : records) {
        if (record.name.size() > std::numeric_limits<uint16_t>::max() ||
            !WriteZipU32(output, 0x02014b50) || !WriteZipU16(output, 45) || !WriteZipU16(output, 45) ||
            !WriteZipU16(output, 0x0800) || !WriteZipU16(output, 8) || !WriteZipU16(output, 0) ||
            !WriteZipU16(output, 0x0021) || !WriteZipU32(output, record.crc) ||
            !WriteZipU32(output, 0xffffffff) || !WriteZipU32(output, 0xffffffff) ||
            !WriteZipU16(output, static_cast<uint16_t>(record.name.size())) || !WriteZipU16(output, 28) ||
            !WriteZipU16(output, 0) || !WriteZipU16(output, 0) || !WriteZipU16(output, 0) ||
            !WriteZipU32(output, 0) || !WriteZipU32(output, 0xffffffff)) return false;
        output.write(record.name.data(), static_cast<std::streamsize>(record.name.size()));
        if (!WriteZipU16(output, 1) || !WriteZipU16(output, 24) ||
            !WriteZipU64(output, record.uncompressedSize) || !WriteZipU64(output, record.compressedSize) ||
            !WriteZipU64(output, record.localOffset)) return false;
    }
    const auto directoryEnd = output.tellp();
    if (directoryEnd == std::streampos(-1)) return false;
    const uint64_t directoryOffset = static_cast<uint64_t>(static_cast<std::streamoff>(directoryStart));
    const uint64_t directorySize = static_cast<uint64_t>(static_cast<std::streamoff>(directoryEnd - directoryStart));
    const uint64_t zip64Offset = static_cast<uint64_t>(static_cast<std::streamoff>(directoryEnd));
    const uint64_t count = records.size();

    return WriteZipU32(output, 0x06064b50) && WriteZipU64(output, 44) &&
           WriteZipU16(output, 45) && WriteZipU16(output, 45) &&
           WriteZipU32(output, 0) && WriteZipU32(output, 0) &&
           WriteZipU64(output, count) && WriteZipU64(output, count) &&
           WriteZipU64(output, directorySize) && WriteZipU64(output, directoryOffset) &&
           WriteZipU32(output, 0x07064b50) && WriteZipU32(output, 0) &&
           WriteZipU64(output, zip64Offset) && WriteZipU32(output, 1) &&
           WriteZipU32(output, 0x06054b50) && WriteZipU16(output, 0) && WriteZipU16(output, 0) &&
           WriteZipU16(output, 0xffff) && WriteZipU16(output, 0xffff) &&
           WriteZipU32(output, 0xffffffff) && WriteZipU32(output, 0xffffffff) && WriteZipU16(output, 0);
}

bool CreateModArchive(const std::vector<pzmm::ModEntry>& mods,
                      const pzmm::fs::path& outputFolder,
                      const std::atomic<bool>& cancel,
                      const pzmm::ProgressCallback& progress,
                      pzmm::fs::path& archivePath,
                      std::string& error) {
    std::error_code ec;
    pzmm::fs::create_directories(outputFolder, ec);
    if (ec) {
        error = "Could not create archive folder: " + ec.message();
        return false;
    }

    std::string stem = mods.size() == 1 ? mods.front().folderName : "Mods archive";
    if (stem.empty()) stem = "Mod archive";
    archivePath = outputFolder / pzmm::fs::u8path(stem + ".zip");
    for (size_t index = 2; pzmm::fs::exists(archivePath, ec) && !ec; ++index) {
        archivePath = outputFolder / pzmm::fs::u8path(stem + " (" + std::to_string(index) + ").zip");
    }
    if (ec) {
        error = "Could not check for an existing archive: " + ec.message();
        return false;
    }

    std::vector<ArchiveFile> files;
    std::set<std::string> archiveRoots;
    for (const auto& mod : mods) {
        if (cancel.load()) {
            error = "Archive creation was cancelled.";
            return false;
        }
        std::string root = mod.folderName.empty() ? "mod" : mod.folderName;
        const std::string baseRoot = root;
        for (size_t suffix = 2; !archiveRoots.insert(root).second; ++suffix)
            root = baseRoot + " (" + std::to_string(suffix) + ")";

        std::error_code walkError;
        pzmm::fs::recursive_directory_iterator it(
            mod.folderPath, pzmm::fs::directory_options::skip_permission_denied, walkError), end;
        for (; !walkError && it != end && !cancel.load(); it.increment(walkError)) {
            std::error_code statusError;
            const auto status = it->symlink_status(statusError);
            if (statusError || !pzmm::fs::is_regular_file(status)) continue;

            const auto relative = it->path().lexically_relative(mod.folderPath);
            std::string relativeName = pzmm::PathToUtf8(relative);
            std::replace(relativeName.begin(), relativeName.end(), '\\', '/');
            files.push_back({it->path(), root + "/" + relativeName});
        }
        if (cancel.load()) {
            error = "Archive creation was cancelled.";
            return false;
        }
        if (walkError) {
            error = "Could not scan " + pzmm::PathToUtf8(mod.folderPath) + ": " + walkError.message();
            return false;
        }
    }
    if (files.empty()) {
        error = "No files were found in the selected mod folder(s).";
        return false;
    }

    std::error_code tempError;
    const auto tempRoot = pzmm::fs::temp_directory_path(tempError) /
        pzmm::fs::u8path("PZModManagerZip-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (tempError) {
        error = "Could not locate a temporary folder for ZIP compression.";
        return false;
    }
    pzmm::fs::create_directories(tempRoot, tempError);
    if (tempError) {
        error = "Could not create temporary compression files: " + tempError.message();
        return false;
    }
    struct TempCleanup {
        pzmm::fs::path path;
        ~TempCleanup() { std::error_code ec; pzmm::fs::remove_all(path, ec); }
    } cleanup{tempRoot};

    const auto markerPath = tempRoot / "archive-marker.txt";
    {
        std::ofstream marker(markerPath, std::ios::binary | std::ios::trunc);
        marker << kModArchiveMarkerText;
        if (!marker) {
            error = "Could not write the mod archive marker.";
            return false;
        }
    }
    files.insert(files.begin(), {markerPath, kModArchiveMarkerPath});

    std::ofstream output(archivePath, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "Could not create " + pzmm::PathToUtf8(archivePath) + ".";
        return false;
    }

    const unsigned int detectedThreads = std::thread::hardware_concurrency();
    const size_t hardwareThreads = detectedThreads == 0 ? 2 : detectedThreads;
    const size_t maxCompressionWorkers = hardwareThreads > 1 ? hardwareThreads - 1 : 0;
    std::vector<ZipCentralRecord> records;
    records.reserve(files.size());
    size_t archivedFiles = 0;
    bool writerFailed = false;

    const auto compressAndWrite = [&](size_t index) {
        CompressedArchiveFile compressed;
        const auto compressedPath = tempRoot / (std::to_string(index) + ".deflate");
        if (!CompressArchiveFile(files[index], compressedPath, cancel, nullptr, nullptr, compressed)) {
            error = compressed.error;
            return false;
        }
        if (progress) progress("Archiving: " + files[index].entryName);
        ZipCentralRecord record;
        if (!WriteZipEntry(output, compressed, record)) {
            error = "Could not write ZIP entry " + files[index].entryName + ".";
            return false;
        }
        records.push_back(std::move(record));
        ++archivedFiles;
        std::error_code removeError;
        pzmm::fs::remove(compressedPath, removeError);
        return true;
    };

    const size_t workerCount = std::min(files.size(), maxCompressionWorkers);
    if (workerCount == 0) {
        for (size_t i = 0; i < files.size() && !cancel.load(); ++i) {
            if (!compressAndWrite(i)) {
                writerFailed = true;
                break;
            }
        }
    } else {
        CompressionScheduler scheduler(workerCount);
        std::atomic<size_t> nextFile{0};
        std::atomic<bool> stopWorkers{false};
        std::mutex readyMutex;
        std::condition_variable readyCondition;
        std::deque<CompressedArchiveFile> ready;
        size_t activeWorkers = workerCount;

        const auto worker = [&]() {
            while (scheduler.Acquire(cancel, stopWorkers)) {
                const size_t index = nextFile.fetch_add(1);
                if (index >= files.size()) {
                    scheduler.Release();
                    break;
                }
                CompressedArchiveFile result;
                const auto compressedPath = tempRoot / (std::to_string(index) + ".deflate");
                CompressArchiveFile(files[index], compressedPath, cancel, &scheduler, &stopWorkers, result);
                scheduler.Release();
                {
                    std::lock_guard<std::mutex> lock(readyMutex);
                    ready.push_back(std::move(result));
                }
                readyCondition.notify_one();
            }
            {
                std::lock_guard<std::mutex> lock(readyMutex);
                --activeWorkers;
            }
            readyCondition.notify_one();
        };

        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (size_t i = 0; i < workerCount; ++i) workers.emplace_back(worker);

        while (true) {
            CompressedArchiveFile result;
            {
                std::unique_lock<std::mutex> lock(readyMutex);
                readyCondition.wait(lock, [&]() { return !ready.empty() || activeWorkers == 0; });
                if (ready.empty() && activeWorkers == 0) break;
                result = std::move(ready.front());
                ready.pop_front();
            }
            if (!writerFailed && !cancel.load()) {
                if (!result.error.empty()) {
                    error = result.error;
                    writerFailed = true;
                    stopWorkers.store(true);
                    scheduler.Wake();
                } else {
                    if (progress) progress("Archiving: " + result.file.entryName);
                    ZipCentralRecord record;
                    if (!WriteZipEntry(output, result, record)) {
                        error = "Could not write ZIP entry " + result.file.entryName + ".";
                        writerFailed = true;
                        stopWorkers.store(true);
                        scheduler.Wake();
                    } else {
                        records.push_back(std::move(record));
                        ++archivedFiles;
                        std::error_code removeError;
                        pzmm::fs::remove(result.compressedPath, removeError);
                    }
                }
            }
        }
        stopWorkers.store(true);
        scheduler.Wake();
        for (auto& thread : workers) thread.join();
    }

    if (cancel.load()) error = "Archive creation was cancelled.";
    if (error.empty() && (writerFailed || archivedFiles != files.size()))
        error = "Not all files could be compressed into the archive.";
    if (error.empty() && !WriteZipCentralDirectory(output, records))
        error = "Could not finalize the ZIP central directory.";
    output.flush();
    if (error.empty() && !output) error = "Finalizing the ZIP archive failed.";
    output.close();
    if (error.empty() && !output) error = "Could not close the ZIP archive.";
    if (!error.empty()) {
        std::error_code removeError;
        pzmm::fs::remove(archivePath, removeError);
        return false;
    }
    const auto verification = InspectModArchive(archivePath);
    if (!verification.marked || verification.modRoots.size() != archiveRoots.size()) {
        error = "The completed ZIP failed its archive-marker/entry verification.";
        std::error_code removeError;
        pzmm::fs::remove(archivePath, removeError);
        return false;
    }
    return true;
}

} // namespace

MainFrame::ImportPlan::~ImportPlan() {
    if (!temporaryRoot.empty()) {
        std::error_code ec;
        pzmm::fs::remove_all(temporaryRoot, ec);
    }
}

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "Project Zomboid Mod Manager", wxDefaultPosition, wxSize(1280, 820)),
      m_progressTimer(this), m_selectionTimer(this) {
    SetMinSize(wxSize(960, 620));

    auto* root = new wxPanel(this);
    auto* rootSizer = new wxBoxSizer(wxVERTICAL);

    auto* headerSizer = new wxBoxSizer(wxHORIZONTAL);
    auto* title = new wxStaticText(root, wxID_ANY, "Project Zomboid Mod Manager");
    wxFont titleFont = title->GetFont();
    titleFont.SetPointSize(titleFont.GetPointSize() + 4);
    titleFont.SetWeight(wxFONTWEIGHT_BOLD);
    title->SetFont(titleFont);
    headerSizer->Add(title, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
    m_importButton = new wxButton(root, wxID_ANY, "Import ZIP...");
    m_importButton->SetBitmap(wxArtProvider::GetBitmap(wxART_FILE_OPEN, wxART_BUTTON, wxSize(16, 16)));
    headerSizer->Add(m_importButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    m_refreshButton = new wxButton(root, wxID_ANY, "Refresh scan");
    m_refreshButton->SetBitmap(wxArtProvider::GetBitmap(wxART_REFRESH, wxART_BUTTON, wxSize(16, 16)));
    headerSizer->Add(m_refreshButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    rootSizer->Add(headerSizer, 0, wxEXPAND | wxALL, 8);

    auto* pathSizer = new wxFlexGridSizer(2, 3, 6, 8);
    pathSizer->AddGrowableCol(1, 1);
    pathSizer->Add(new wxStaticText(root, wxID_ANY, "User mods folder:"), 0, wxALIGN_CENTER_VERTICAL);
    m_userPathCtrl = new wxTextCtrl(root, wxID_ANY, DefaultUserModsPath());
    pathSizer->Add(m_userPathCtrl, 1, wxEXPAND);
    m_browseUserButton = new wxButton(root, wxID_ANY, "Browse...");
    m_browseUserButton->SetBitmap(wxArtProvider::GetBitmap(wxART_FOLDER, wxART_BUTTON, wxSize(16, 16)));
    pathSizer->Add(m_browseUserButton, 0);

    pathSizer->Add(new wxStaticText(root, wxID_ANY, "Steam Workshop folder:"), 0, wxALIGN_CENTER_VERTICAL);
    m_workshopPathCtrl = new wxTextCtrl(root, wxID_ANY, DefaultWorkshopPath());
    pathSizer->Add(m_workshopPathCtrl, 1, wxEXPAND);
    m_browseWorkshopButton = new wxButton(root, wxID_ANY, "Browse...");
    m_browseWorkshopButton->SetBitmap(wxArtProvider::GetBitmap(wxART_FOLDER, wxART_BUTTON, wxSize(16, 16)));
    pathSizer->Add(m_browseWorkshopButton, 0);
    rootSizer->Add(pathSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    m_notebook = new wxNotebook(root, wxID_ANY);
    m_userPage = BuildListPage(PageKind::UserMods);
    m_workshopPage = BuildListPage(PageKind::WorkshopMods);
    m_notebook->AddPage(m_userPage.panel, "User mods");
    m_notebook->AddPage(m_workshopPage.panel, "Steam Workshop");
    rootSizer->Add(m_notebook, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    m_activityText = new wxStaticText(root, wxID_ANY, "Ready.");
    rootSizer->Add(m_activityText, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
    m_progressGauge = new wxGauge(root, wxID_ANY, 100, wxDefaultPosition, wxDefaultSize, wxGA_HORIZONTAL);
    m_progressGauge->SetValue(0);
    rootSizer->Add(m_progressGauge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    root->SetSizer(rootSizer);
    m_statusBar = CreateStatusBar(2);
    int statusWidths[] = {-1, 300};
    m_statusBar->SetFieldsCount(2, statusWidths);
    m_statusBar->SetStatusText("Ready. Select Refresh scan to read the mod folders.", 0);
    m_statusBar->SetStatusText("Idle", 1);

    m_browseUserButton->Bind(wxEVT_BUTTON, &MainFrame::OnBrowseUserPath, this);
    m_browseWorkshopButton->Bind(wxEVT_BUTTON, &MainFrame::OnBrowseWorkshopPath, this);
    m_importButton->Bind(wxEVT_BUTTON, &MainFrame::OnImportArchive, this);
    m_refreshButton->Bind(wxEVT_BUTTON, &MainFrame::OnRefresh, this);
    Bind(wxEVT_PZMM_JOB_DONE, &MainFrame::OnJobDone, this);
    Bind(wxEVT_PZMM_PROGRESS, &MainFrame::OnProgress, this);
    Bind(wxEVT_TIMER, &MainFrame::OnProgressPulse, this, m_progressTimer.GetId());
    Bind(wxEVT_TIMER, &MainFrame::OnSelectionSettled, this, m_selectionTimer.GetId());
    Bind(wxEVT_SYS_COLOUR_CHANGED, &MainFrame::OnSystemColourChanged, this);
    ApplySystemTheme();

    StartScan();
}

MainFrame::~MainFrame() {
    m_shuttingDown.store(true);
    m_progressTimer.Stop();
    m_selectionTimer.Stop();
    m_cancel.store(true);
    if (m_worker.joinable()) m_worker.join();
}

MainFrame::PageWidgets MainFrame::BuildListPage(PageKind kind) {
    PageWidgets widgets;
    widgets.panel = new wxPanel(m_notebook);
    auto* outerSizer = new wxBoxSizer(wxVERTICAL);
    auto* contentSizer = new wxBoxSizer(wxHORIZONTAL);

    widgets.filterBar = new wxScrolledWindow(widgets.panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                              wxHSCROLL | wxBORDER_NONE);
    widgets.filterBar->SetScrollRate(8, 0);
    widgets.filterBar->SetMinSize(wxSize(-1, 34));
    widgets.filterSizer = new wxBoxSizer(wxHORIZONTAL);
    widgets.filterBar->SetSizer(widgets.filterSizer);
    outerSizer->Add(widgets.filterBar, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 5);

    widgets.list = new wxListCtrl(widgets.panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                  wxLC_REPORT | wxLC_HRULES | wxLC_VRULES);
    widgets.list->InsertColumn(0, "Mod name", wxLIST_FORMAT_LEFT, 190);
    widgets.list->InsertColumn(1, "Mod ID", wxLIST_FORMAT_LEFT, 165);
    widgets.list->InsertColumn(2, "Game versions", wxLIST_FORMAT_LEFT, 110);
    widgets.list->InsertColumn(3, "State", wxLIST_FORMAT_LEFT, 120);
    if (kind == PageKind::WorkshopMods) {
        widgets.list->InsertColumn(4, "Locally installed", wxLIST_FORMAT_LEFT, 120);
        widgets.list->InsertColumn(5, "Workshop ID", wxLIST_FORMAT_LEFT, 100);
        widgets.list->InsertColumn(6, "Folder modified", wxLIST_FORMAT_LEFT, 135);
        widgets.columnTitles = {"Mod name", "Mod ID", "Game versions", "State",
                                "Locally installed", "Workshop ID", "Folder modified"};
    } else {
        widgets.list->InsertColumn(4, "Folder modified", wxLIST_FORMAT_LEFT, 135);
        widgets.list->InsertColumn(5, "Workshop update", wxLIST_FORMAT_LEFT, 150);
        widgets.columnTitles = {"Mod name", "Mod ID", "Game versions", "State", "Folder modified",
                                "Workshop update"};
    }
    contentSizer->Add(widgets.list, 3, wxEXPAND | wxALL, 5);

    auto* detailPanel = new wxPanel(widgets.panel);
    auto* detailSizer = new wxBoxSizer(wxVERTICAL);
    widgets.poster = new wxStaticBitmap(detailPanel, wxID_ANY, wxBitmap(1, 1));
    widgets.poster->SetMinSize(wxSize(180, 170));
    detailSizer->Add(widgets.poster, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, 6);
    widgets.details = new wxTextCtrl(detailPanel, wxID_ANY, "Select a mod to inspect its metadata.",
                                     wxDefaultPosition, wxDefaultSize,
                                     wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP | wxBORDER_SIMPLE);
    detailSizer->Add(widgets.details, 1, wxEXPAND | wxALL, 5);
    detailPanel->SetSizer(detailSizer);
    contentSizer->Add(detailPanel, 2, wxEXPAND | wxALL, 5);
    outerSizer->Add(contentSizer, 1, wxEXPAND);

    if (kind == PageKind::WorkshopMods) {
        auto* actionSizer = new wxBoxSizer(wxHORIZONTAL);
        actionSizer->Add(new wxStaticText(widgets.panel, wxID_ANY, "Update comparison:"), 0,
                         wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 5);
        m_updateMethodChoice = new wxChoice(widgets.panel, wxID_ANY);
        m_updateMethodChoice->Append("mod.info version / game folders");
        m_updateMethodChoice->Append("Newest file modification time");
        m_updateMethodChoice->SetSelection(0);
        actionSizer->Add(m_updateMethodChoice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_copyButton = new wxButton(widgets.panel, wxID_ANY, "Copy Selected");
        m_copyButton->SetBitmap(wxArtProvider::GetBitmap(wxART_COPY, wxART_BUTTON, wxSize(16, 16)));
        m_updateButton = new wxButton(widgets.panel, wxID_ANY, "Update if Newer");
        m_updateButton->SetBitmap(wxArtProvider::GetBitmap(wxART_REDO, wxART_BUTTON, wxSize(16, 16)));
        actionSizer->Add(m_copyButton, 0, wxRIGHT, 6);
        actionSizer->Add(m_updateButton, 0, wxRIGHT, 5);
        outerSizer->Add(actionSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        m_copyButton->Bind(wxEVT_BUTTON, &MainFrame::OnCopySelected, this);
        m_updateButton->Bind(wxEVT_BUTTON, &MainFrame::OnUpdateSelected, this);
    } else {
        auto* actionSizer = new wxBoxSizer(wxHORIZONTAL);
        actionSizer->Add(new wxStaticText(widgets.panel, wxID_ANY, "Update comparison:"), 0,
                         wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 5);
        m_userUpdateMethodChoice = new wxChoice(widgets.panel, wxID_ANY);
        m_userUpdateMethodChoice->Append("mod.info version / game folders");
        m_userUpdateMethodChoice->Append("Newest file modification time");
        m_userUpdateMethodChoice->SetSelection(0);
        actionSizer->Add(m_userUpdateMethodChoice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_updateUserButton = new wxButton(widgets.panel, wxID_ANY, "Update selected if newer");
        m_updateUserButton->SetBitmap(wxArtProvider::GetBitmap(wxART_REDO, wxART_BUTTON, wxSize(16, 16)));
        actionSizer->Add(m_updateUserButton, 0, wxRIGHT, 5);
        outerSizer->Add(actionSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        m_updateUserButton->Bind(wxEVT_BUTTON, &MainFrame::OnUpdateUserSelected, this);
    }

    widgets.panel->SetSizer(outerSizer);
    widgets.list->Bind(wxEVT_LIST_COL_CLICK, [this, kind](wxListEvent& event) {
        OnColumnClick(kind, event);
    });
    widgets.list->Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, [this, kind](wxListEvent& event) {
        OnListRightClick(kind, event);
    });
    widgets.list->Bind(wxEVT_LIST_ITEM_SELECTED, [this, kind](wxListEvent& event) {
        OnListSelection(kind, event);
    });
    widgets.list->Bind(wxEVT_LIST_ITEM_DESELECTED, [this, kind](wxListEvent&) {
        PageWidgets& page = kind == PageKind::UserMods ? m_userPage : m_workshopPage;
        if (page.list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED) < 0) {
            page.details->ChangeValue("Select a mod to inspect its metadata.");
            SetPoster(page, {});
        }
    });
    return widgets;
}

void MainFrame::StartScan() {
    if (m_busy) return;
    const auto userRoot = ToFsPath(m_userPathCtrl->GetValue());
    const auto workshopRoot = ToFsPath(m_workshopPathCtrl->GetValue());
    StartWorker([this, userRoot, workshopRoot](const std::atomic<bool>& cancel) {
        JobResult result;
        result.scan = pzmm::ScanAll(userRoot, workshopRoot, cancel,
            [this](const std::string& text) { ReportProgress(text); }, MetadataCacheDirectory());
        result.hasScan = true;
        result.success = true;
        result.message = "Scan complete.";
        return result;
    });
}

void MainFrame::StartWorker(std::function<JobResult(const std::atomic<bool>&)> task) {
    if (m_busy) return;
    if (m_worker.joinable()) m_worker.join();
    m_cancel.store(false);
    SetBusy(true);
    m_statusBar->SetStatusText("Working in the background...", 0);
    m_statusBar->SetStatusText("Scanning", 1);
    m_activityText->SetLabel("Starting background operation...");
    m_progressGauge->Pulse();
    m_progressTimer.Start(100);
    m_worker = std::thread([this, task = std::move(task)]() mutable {
        JobResult result;
        try {
            result = task(m_cancel);
        } catch (const std::exception& error) {
            result.success = false;
            result.showMessage = true;
            result.message = std::string("Background task failed: ") + error.what();
        } catch (...) {
            result.success = false;
            result.showMessage = true;
            result.message = "Background task failed with an unknown error.";
        }
        if (m_shuttingDown.load()) return;
        auto* event = new wxThreadEvent(wxEVT_PZMM_JOB_DONE);
        event->SetPayload(std::make_shared<JobResult>(std::move(result)));
        wxQueueEvent(this, event);
    });
}

void MainFrame::ReportProgress(const std::string& message) {
    if (m_shuttingDown.load()) return;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    long long previous = m_lastProgressReportMs.load();
    if (previous != 0 && now - previous < 100) return;
    if (!m_lastProgressReportMs.compare_exchange_strong(previous, now)) return;
    auto* event = new wxThreadEvent(wxEVT_PZMM_PROGRESS);
    event->SetPayload(message);
    wxQueueEvent(this, event);
}

void MainFrame::OnProgress(wxThreadEvent& event) {
    const std::string progress = event.GetPayload<std::string>();
    wxString message = FromUtf8(progress);
    if (progress.compare(0, 14, "COPY_PROGRESS:") == 0) {
        m_progressTimer.Stop(); // determinate percentage while copying files
        const size_t firstColon = progress.find(':');
        const size_t secondColon = progress.find(':', firstColon + 1);
        const size_t separator = progress.find('|', secondColon + 1);
        if (firstColon != std::string::npos && secondColon != std::string::npos &&
            separator != std::string::npos) {
            try {
                const unsigned long long done = std::stoull(
                    progress.substr(firstColon + 1, secondColon - firstColon - 1));
                const unsigned long long total = std::max(1ULL, std::stoull(
                    progress.substr(secondColon + 1, separator - secondColon - 1)));
                const auto percentage = static_cast<int>(std::min(100.0L,
                    static_cast<long double>(done) * 100.0L / static_cast<long double>(total)));
                m_progressGauge->SetValue(percentage);
                message = FromUtf8(progress.substr(separator + 1));
            } catch (const std::exception&) {
                message = FromUtf8(progress);
            }
        }
    }
    else if (m_busy && !m_progressTimer.IsRunning()) {
        m_progressGauge->SetValue(0);
        m_progressTimer.Start(100); // scanning has no reliable total, so pulse indeterminately
    }
    if (!message.empty()) {
        m_activityText->SetLabel(message);
        m_statusBar->SetStatusText(message, 0);
        m_statusBar->SetStatusText("Working", 1);
    }
}

void MainFrame::OnProgressPulse(wxTimerEvent&) {
    if (m_busy && m_progressGauge) m_progressGauge->Pulse();
}

void MainFrame::OnSystemColourChanged(wxSysColourChangedEvent& event) {
    event.Skip();
    ApplySystemTheme();
}

void MainFrame::ApplySystemTheme() {
    const bool dark = wxSystemSettings::GetAppearance().IsDark();
    const wxColour background = dark ? wxColour(32, 32, 32) : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    const wxColour foreground = dark ? wxColour(240, 240, 240) : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    std::function<void(wxWindow*)> apply = [&](wxWindow* window) {
        window->SetBackgroundColour(background);
        window->SetForegroundColour(foreground);
        for (auto child = window->GetChildren().GetFirst(); child; child = child->GetNext())
            apply(child->GetData());
        window->Refresh(false);
    };
    apply(this);
}

void MainFrame::OnColumnClick(PageKind kind, wxListEvent& event) {
    PageWidgets* page = kind == PageKind::UserMods ? &m_userPage : &m_workshopPage;
    const int col = event.GetColumn();
    if (col < 0 || static_cast<size_t>(col) >= page->columnTitles.size()) return;
    if (page->sortColumn == col) page->sortAscending = !page->sortAscending;
    else { page->sortColumn = col; page->sortAscending = true; }
    const auto& mods = kind == PageKind::UserMods ? m_scanResults.userMods : m_scanResults.workshopMods;
    RefreshListView(kind, *page, mods);
}

void MainFrame::OnListRightClick(PageKind kind, wxListEvent& event) {
    PageWidgets& page = kind == PageKind::UserMods ? m_userPage : m_workshopPage;
    const long clickedRow = event.GetIndex();
    if (clickedRow < 0) return;
    if (!(page.list->GetItemState(clickedRow, wxLIST_STATE_SELECTED) & wxLIST_STATE_SELECTED)) {
        for (long row = page.list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
             row >= 0; row = page.list->GetNextItem(row, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) {
            page.list->SetItemState(row, 0, wxLIST_STATE_SELECTED);
        }
        page.list->SetItemState(clickedRow, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    }

    const auto& mods = kind == PageKind::UserMods ? m_scanResults.userMods : m_scanResults.workshopMods;
    const size_t sourceIndex = static_cast<size_t>(page.list->GetItemData(clickedRow));
    if (sourceIndex >= mods.size()) return;
    const auto folderPath = mods[sourceIndex].folderPath;
    wxMenu menu;
    auto* revealItem = menu.Append(7200, "Show in Explorer");
    revealItem->SetBitmap(wxArtProvider::GetBitmap(wxART_FOLDER_OPEN, wxART_MENU, wxSize(16, 16)));
    auto* archiveItem = menu.Append(7204, "Archive selected mod(s) to ZIP...");
    archiveItem->SetBitmap(wxArtProvider::GetBitmap(wxART_FILE_SAVE, wxART_MENU, wxSize(16, 16)));
    menu.AppendSeparator();
    menu.Bind(wxEVT_MENU, [folderPath](wxCommandEvent&) { OpenInExplorer(folderPath); }, 7200);
    menu.Bind(wxEVT_MENU, [this, kind](wxCommandEvent&) { ArchiveSelectedMods(kind); }, 7204);
    if (kind == PageKind::UserMods) {
        auto* deleteItem = menu.Append(7201, "Delete selected mod(s)...");
        deleteItem->SetBitmap(wxArtProvider::GetBitmap(wxART_DELETE, wxART_MENU, wxSize(16, 16)));
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { DeleteSelectedUserMods(); }, 7201);
    } else {
        auto* copyItem = menu.Append(7202, "Copy selected mod(s)...");
        copyItem->SetBitmap(wxArtProvider::GetBitmap(wxART_COPY, wxART_MENU, wxSize(16, 16)));
        auto* updateItem = menu.Append(7203, "Update selected if newer...");
        updateItem->SetBitmap(wxArtProvider::GetBitmap(wxART_REDO, wxART_MENU, wxSize(16, 16)));
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { StartCopyOrUpdate(false); }, 7202);
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { StartCopyOrUpdate(true); }, 7203);
    }
    PopupMenu(&menu);
}

wxString MainFrame::ColumnValue(PageKind kind, const pzmm::ModEntry& mod, int column) const {
    switch (column) {
    case 0: return FromUtf8(mod.name);
    case 1: return FromUtf8(mod.id);
    case 2: return FromUtf8(VersionsLabel(mod));
    case 3:
        return FromUtf8(EntryLoadStatus(mod));
    case 4:
        if (kind == PageKind::WorkshopMods) {
            return mod.localMatchAmbiguous ? "Ambiguous ID" : (mod.locallyInstalled ? "Installed" : "Missing");
        }
        return FolderModifiedLabel(mod);
    case 5:
        return kind == PageKind::WorkshopMods ? FromUtf8(mod.workshopItemId)
            : FromUtf8(mod.workshopCounterpartFound
                ? (mod.workshopUpdateAvailable ? "Update available" : "No newer Workshop version")
                : "No Workshop match");
    case 6:
        return kind == PageKind::WorkshopMods ? FolderModifiedLabel(mod) : wxString{};
    default: return {};
    }
}

bool MainFrame::IsPresetFilterColumn(PageKind kind, int column) const {
    return column == 2 || column == 3 ||
           (kind == PageKind::WorkshopMods ? column == 4 : column == 5);
}

std::vector<std::pair<std::string, wxString>> MainFrame::PresetFilterOptions(PageKind kind, int column) const {
    const auto& mods = kind == PageKind::UserMods ? m_scanResults.userMods : m_scanResults.workshopMods;
    std::map<std::string, wxString> options;
    if (column == 2) {
        for (const auto& mod : mods) {
            if (mod.gameVersions.empty()) {
                options.emplace("version:none", "No game-version folders");
                continue;
            }
            for (const auto& version : mod.gameVersions) {
                const std::string normalized = LowerFilterText(version);
                options.emplace("version:exact:" + normalized, FromUtf8(version + " (exact)"));
                const auto parts = NumericVersionParts(version);
                if (!parts.empty()) {
                    options.emplace("version:major:" + parts[0], FromUtf8(parts[0] + " (all subversions)"));
                    if (parts.size() > 1) {
                        const std::string minor = parts[0] + "." + parts[1];
                        options.emplace("version:minor:" + minor, FromUtf8(minor + " (all subversions)"));
                    }
                }
            }
        }
    } else if (column == 3) {
        for (const auto& mod : mods) {
            if (mod.id.empty()) options.emplace("state:no-id", "No mod.info ID");
            else if (mod.loadOrder > 0) options.emplace("state:enabled", "Enabled");
            else options.emplace("state:not-enabled", "Not enabled");
        }
    } else if (kind == PageKind::WorkshopMods && column == 4) {
        for (const auto& mod : mods) {
            if (mod.localMatchAmbiguous) options.emplace("installed:ambiguous", "Ambiguous ID");
            else if (mod.locallyInstalled) options.emplace("installed:yes", "Installed");
            else options.emplace("installed:no", "Missing");
        }
    } else if (kind == PageKind::UserMods && column == 5) {
        for (const auto& mod : mods) {
            if (!mod.workshopCounterpartFound) options.emplace("update:no-match", "No Workshop match");
            else if (mod.workshopUpdateAvailable) options.emplace("update:available", "Update available");
            else options.emplace("update:current", "No newer Workshop version");
        }
    }

    return {options.begin(), options.end()};
}

bool MainFrame::MatchesPresetFilter(PageKind kind, const pzmm::ModEntry& mod, int column,
                                    const std::set<std::string>& selected) const {
    if (column == 2) {
        for (const auto& version : mod.gameVersions) {
            const std::string normalized = LowerFilterText(version);
            if (selected.count("version:exact:" + normalized)) return true;
            for (const auto& key : selected) {
                constexpr const char* majorPrefix = "version:major:";
                constexpr const char* minorPrefix = "version:minor:";
                if (key.compare(0, std::char_traits<char>::length(majorPrefix), majorPrefix) == 0 &&
                    VersionMatchesBucket(version, key.substr(std::char_traits<char>::length(majorPrefix))))
                    return true;
                if (key.compare(0, std::char_traits<char>::length(minorPrefix), minorPrefix) == 0 &&
                    VersionMatchesBucket(version, key.substr(std::char_traits<char>::length(minorPrefix))))
                    return true;
            }
        }
        return mod.gameVersions.empty() && selected.count("version:none");
    }

    std::string key;
    if (column == 3) {
        key = mod.id.empty() ? "state:no-id" : (mod.loadOrder > 0 ? "state:enabled" : "state:not-enabled");
    } else if (kind == PageKind::WorkshopMods && column == 4) {
        key = mod.localMatchAmbiguous ? "installed:ambiguous" : (mod.locallyInstalled ? "installed:yes" : "installed:no");
    } else if (kind == PageKind::UserMods && column == 5) {
        key = !mod.workshopCounterpartFound ? "update:no-match"
            : (mod.workshopUpdateAvailable ? "update:available" : "update:current");
    }
    return !key.empty() && selected.count(key) != 0;
}

void MainFrame::RefreshFilterBar(PageKind kind, PageWidgets& page) {
    page.filterSizer->Clear(true);
    for (size_t column = 0; column < page.columnTitles.size(); ++column) {
        const int index = static_cast<int>(column);
        const auto textFilter = page.filters.find(index);
        const auto valueFilter = page.valueFilters.find(index);
        if (textFilter == page.filters.end() && valueFilter == page.valueFilters.end()) continue;

        wxString value;
        if (textFilter != page.filters.end()) {
            value = textFilter->second;
        } else if (valueFilter->second.size() == 1) {
            const auto options = page.presetOptions.find(index);
            if (options != page.presetOptions.end()) {
                for (const auto& option : options->second) {
                    if (valueFilter->second.count(option.first)) {
                        value = option.second;
                        break;
                    }
                }
            }
        } else {
            value = FromUtf8(std::to_string(valueFilter->second.size()) + " selected");
        }

        auto* chip = new wxPanel(page.filterBar);
        auto* chipSizer = new wxBoxSizer(wxHORIZONTAL);
        auto* openButton = new wxButton(chip, wxID_ANY, page.columnTitles[index] + ": " + value,
                                        wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        openButton->SetBitmap(wxArtProvider::GetBitmap(wxART_GO_DOWN, wxART_BUTTON, wxSize(12, 12)));
        openButton->SetBitmapPosition(wxRIGHT);
        chipSizer->Add(openButton, 0, wxALIGN_CENTER_VERTICAL);
        auto* closeButton = new wxButton(chip, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(24, 24), wxBU_EXACTFIT);
        closeButton->SetBitmap(wxArtProvider::GetBitmap(wxART_CROSS_MARK, wxART_BUTTON, wxSize(12, 12)));
        closeButton->SetToolTip("Remove this filter");
        chipSizer->Add(closeButton, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 2);
        chip->SetSizer(chipSizer);
        page.filterSizer->Add(chip, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);

        PageWidgets* pagePointer = &page;
        openButton->Bind(wxEVT_BUTTON, [this, kind, pagePointer, index, openButton](wxCommandEvent&) {
            if (IsPresetFilterColumn(kind, index)) ShowPresetFilterMenu(kind, *pagePointer, index, openButton);
            else EditTextFilter(kind, *pagePointer, index);
        });
        closeButton->Bind(wxEVT_BUTTON, [this, kind, pagePointer, index](wxCommandEvent&) {
            pagePointer->filters.erase(index);
            pagePointer->valueFilters.erase(index);
            const auto& mods = kind == PageKind::UserMods ? m_scanResults.userMods : m_scanResults.workshopMods;
            RefreshListView(kind, *pagePointer, mods);
            CallAfter([this, kind, pagePointer]() { RefreshFilterBar(kind, *pagePointer); });
        });
    }

    auto* addButton = new wxButton(page.filterBar, wxID_ANY, "Add filter", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    addButton->SetBitmap(wxArtProvider::GetBitmap(wxART_PLUS, wxART_BUTTON, wxSize(14, 14)));
    addButton->Bind(wxEVT_BUTTON, [this, kind, pagePointer = &page, addButton](wxCommandEvent&) {
        ShowAddFilterMenu(kind, *pagePointer, addButton);
    });
    page.filterSizer->Add(addButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    page.filterBar->FitInside();
    page.filterBar->Layout();
}

void MainFrame::PopulatePresetFilterMenu(wxMenu& menu, PageKind kind, PageWidgets& page, int column) {
    const auto options = page.presetOptions.find(column);
    if (options == page.presetOptions.end() || options->second.empty()) {
        menu.Append(wxID_ANY, "No values available")->Enable(false);
        return;
    }
    const auto selected = page.valueFilters.find(column);
    static std::atomic<int> nextMenuId{15000};
    for (const auto& option : options->second) {
        const int id = nextMenuId.fetch_add(1);
        auto* item = menu.AppendCheckItem(id, option.second);
        if (selected != page.valueFilters.end() && selected->second.count(option.first)) item->Check(true);
        menu.Bind(wxEVT_MENU, [this, kind, pagePointer = &page, column, key = option.first](wxCommandEvent& event) {
            OnPresetFilterChanged(kind, *pagePointer, column, key, event.IsChecked());
        }, id);
    }
}

void MainFrame::ShowPresetFilterMenu(PageKind kind, PageWidgets& page, int column, wxWindow* parent) {
    wxMenu menu;
    PopulatePresetFilterMenu(menu, kind, page, column);
    parent->PopupMenu(&menu);
}

void MainFrame::OnPresetFilterChanged(PageKind kind, PageWidgets& page, int column,
                                      const std::string& key, bool enabled) {
    auto& selected = page.valueFilters[column];
    if (enabled) selected.insert(key);
    else selected.erase(key);
    if (selected.empty()) page.valueFilters.erase(column);
    const auto& mods = kind == PageKind::UserMods ? m_scanResults.userMods : m_scanResults.workshopMods;
    RefreshListView(kind, page, mods);
    PageWidgets* pagePointer = &page;
    CallAfter([this, kind, pagePointer]() { RefreshFilterBar(kind, *pagePointer); });
}

void MainFrame::ShowAddFilterMenu(PageKind kind, PageWidgets& page, wxWindow* parent) {
    wxMenu menu;
    static std::atomic<int> nextMenuId{25000};
    for (size_t column = 0; column < page.columnTitles.size(); ++column) {
        const int index = static_cast<int>(column);
        if (IsPresetFilterColumn(kind, index)) {
            auto* submenu = new wxMenu();
            PopulatePresetFilterMenu(*submenu, kind, page, index);
            menu.AppendSubMenu(submenu, page.columnTitles[index]);
        } else {
            const int id = nextMenuId.fetch_add(1);
            menu.Append(id, "Filter " + page.columnTitles[index] + "...");
            menu.Bind(wxEVT_MENU, [this, kind, pagePointer = &page, index](wxCommandEvent&) {
                EditTextFilter(kind, *pagePointer, index);
            }, id);
        }
    }
    parent->PopupMenu(&menu);
}

void MainFrame::EditTextFilter(PageKind kind, PageWidgets& page, int column) {
    const auto current = page.filters.find(column);
    const wxString initial = current == page.filters.end() ? wxString{} : current->second;
    wxTextEntryDialog dialog(this, "Show rows where this column contains:",
                             "Filter: " + page.columnTitles[column], initial);
    if (dialog.ShowModal() != wxID_OK) return;
    const wxString value = dialog.GetValue().Lower();
    if (value.empty()) page.filters.erase(column);
    else page.filters[column] = value;
    const auto& mods = kind == PageKind::UserMods ? m_scanResults.userMods : m_scanResults.workshopMods;
    RefreshListView(kind, page, mods);
    PageWidgets* pagePointer = &page;
    CallAfter([this, kind, pagePointer]() { RefreshFilterBar(kind, *pagePointer); });
}

void MainFrame::OnJobDone(wxThreadEvent& event) {
    if (m_worker.joinable()) m_worker.join();
    const auto payload = event.GetPayload<std::shared_ptr<JobResult>>();
    if (!payload) {
        SetBusy(false);
        m_statusBar->SetStatusText("Background task finished without a result.");
        return;
    }

    JobResult result = *payload;
    if (result.importPlan) {
        m_progressTimer.Stop();
        m_progressGauge->SetValue(0);
        m_activityText->SetLabel("Ready.");
        m_statusBar->SetStatusText("Idle", 1);
        SetBusy(false);
        ReviewImportPlan(result.importPlan);
        return;
    }
    if (result.hasScan) {
        m_scanResults = std::move(result.scan);
        m_hasScan = true;
        PopulateAllLists();
        const std::string defaultStatus = m_scanResults.defaultTxtFound
            ? "default.txt: " + std::to_string(m_scanResults.defaultModIds.size()) + " queued mods"
            : "default.txt not found";
        const std::string customStatus = m_scanResults.customTxtWritten
            ? "custom.txt saved"
            : (m_scanResults.customTxtWarning.empty() ? "custom.txt not written" : m_scanResults.customTxtWarning);
        m_statusBar->SetStatusText(FromUtf8(std::string("User mods: ") + std::to_string(m_scanResults.userMods.size()) +
                                   " | Workshop mods: " + std::to_string(m_scanResults.workshopMods.size()) +
                                   " | " + defaultStatus + " | " + customStatus));
    } else {
        m_statusBar->SetStatusText(FromUtf8(result.message));
    }

    m_progressTimer.Stop();
    m_progressGauge->SetValue(0);
    m_activityText->SetLabel(result.success ? "Ready." : "Operation failed.");
    m_statusBar->SetStatusText(result.message.empty() ? wxString("Ready.") : FromUtf8(result.message), 0);
    m_statusBar->SetStatusText("Idle", 1);
    SetBusy(false);
    if (result.showMessage) {
        wxMessageBox(FromUtf8(result.message), result.success ? "Project Zomboid Mod Manager" : "Operation failed",
                     wxOK | (result.success ? wxICON_INFORMATION : wxICON_WARNING), this);
    }
    if (result.showArchiveNotification && result.success) ShowArchiveNotification(result.archivePath);
}

void MainFrame::OnBrowseUserPath(wxCommandEvent&) {
    wxDirDialog dialog(this, "Choose the Project Zomboid user mods folder", m_userPathCtrl->GetValue(),
                       wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dialog.ShowModal() == wxID_OK) m_userPathCtrl->SetValue(dialog.GetPath());
}

void MainFrame::OnBrowseWorkshopPath(wxCommandEvent&) {
    wxDirDialog dialog(this, "Choose the Steam Workshop content/108600 folder", m_workshopPathCtrl->GetValue(),
                       wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dialog.ShowModal() == wxID_OK) m_workshopPathCtrl->SetValue(dialog.GetPath());
}

void MainFrame::OnImportArchive(wxCommandEvent&) {
    if (m_busy) return;
    if (!m_hasScan || !PathsMatchLastScan()) {
        wxMessageBox("Refresh the scan after changing either folder path before importing mods.",
                     "Refresh required", wxOK | wxICON_INFORMATION, this);
        return;
    }

    pzmm::fs::path archivePath;
    ArchiveInspection inspection;
    for (;;) {
        wxFileDialog dialog(this, "Select a mods ZIP archive", wxEmptyString, wxEmptyString,
                            "ZIP archives (*.zip)|*.zip", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) return;
        archivePath = ToFsPath(dialog.GetPath());
        inspection = InspectModArchive(archivePath);
        if (!inspection.error.empty()) {
            wxMessageBox(FromUtf8(inspection.error), "Archive inspection failed", wxOK | wxICON_WARNING, this);
            return;
        }
        if (inspection.marked) break;
        const int answer = wxMessageBox(
            "This ZIP does not contain the Project Zomboid Mod Manager archive marker. "
            "Continue only if you are sure it contains mod folders.\n\n"
            "Choose Yes to inspect it, No to select a different ZIP, or Cancel to stop.",
            "Unrecognized ZIP archive", wxYES_NO | wxCANCEL | wxICON_WARNING, this);
        if (answer == wxYES) break;
        if (answer != wxNO) return;
    }

    std::error_code tempError;
    const auto tempRoot = pzmm::fs::temp_directory_path(tempError) /
        pzmm::fs::u8path("PZModManagerImport-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (tempError) {
        wxMessageBox("Could not locate a temporary folder for archive inspection.",
                     "Import failed", wxOK | wxICON_WARNING, this);
        return;
    }
    const auto userRoot = ToFsPath(m_userPathCtrl->GetValue());
    const auto workshopRoot = ToFsPath(m_workshopPathCtrl->GetValue());
    StartWorker([this, archivePath, inspection = std::move(inspection), tempRoot, userRoot, workshopRoot]
                (const std::atomic<bool>& cancel) mutable {
        JobResult result;
        auto plan = std::make_shared<ImportPlan>();
        plan->temporaryRoot = tempRoot;
        plan->userModsRoot = userRoot;
        plan->workshopRoot = workshopRoot;
        std::string error;
        if (!ExtractModArchive(archivePath, tempRoot, inspection, cancel, error)) {
            result.success = false;
            result.showMessage = true;
            result.message = "Could not inspect the selected mod archive. " + error;
            return result;
        }
        const auto imported = pzmm::ScanAll(tempRoot, {}, cancel,
            [this](const std::string& text) { ReportProgress(text); }, MetadataCacheDirectory());
        plan->mods = imported.userMods;
        if (plan->mods.empty()) {
            result.success = false;
            result.showMessage = true;
            result.message = "The archive did not contain any recognizable mod folders.";
            return result;
        }
        result.message = "Archive inspection complete.";
        result.importPlan = std::move(plan);
        return result;
    });
}

void MainFrame::ReviewImportPlan(const std::shared_ptr<ImportPlan>& plan) {
    if (!plan || plan->mods.empty()) return;

    struct Candidate {
        size_t importedIndex = 0;
        std::optional<pzmm::ModEntry> installed;
        std::optional<int> versionComparison;
    };
    std::vector<Candidate> candidates;
    std::vector<size_t> newMods;
    std::vector<size_t> outdatedMods;
    std::vector<size_t> sameVersionMods;
    std::vector<size_t> olderImports;
    std::vector<size_t> unknownVersions;

    for (size_t i = 0; i < plan->mods.size(); ++i) {
        const auto& imported = plan->mods[i];
        const auto target = pzmm::ResolveTarget(imported, m_scanResults.userMods, plan->userModsRoot);
        Candidate candidate;
        candidate.importedIndex = i;
        if (target.matchedEntry) {
            candidate.installed = *target.matchedEntry;
            candidate.versionComparison = CompareImportModVersions(imported.modVersion, target.matchedEntry->modVersion);
        }
        candidates.push_back(candidate);
        if (!candidate.installed) newMods.push_back(i);
        else if (!candidate.versionComparison) unknownVersions.push_back(i);
        else if (*candidate.versionComparison > 0) outdatedMods.push_back(i);
        else if (*candidate.versionComparison == 0) sameVersionMods.push_back(i);
        else olderImports.push_back(i);
    }

    const wxString summary = "Archive contents: " + FromUtf8(std::to_string(plan->mods.size())) +
        " mod(s). New: " + FromUtf8(std::to_string(newMods.size())) +
        ", newer modversion: " + FromUtf8(std::to_string(outdatedMods.size())) +
        ", same modversion: " + FromUtf8(std::to_string(sameVersionMods.size())) +
        ", older: " + FromUtf8(std::to_string(olderImports.size())) +
        ", version unknown: " + FromUtf8(std::to_string(unknownVersions.size())) + ".";
    wxMessageBox(summary, "Import comparison", wxOK | wxICON_INFORMATION, this);

    bool combineWithExisting = true;
    if (!m_scanResults.userMods.empty()) {
        const int answer = wxMessageBox(
            "The User Mods folder already contains " + FromUtf8(std::to_string(m_scanResults.userMods.size())) +
            " mod(s). Combine this import with the existing mods? Choosing No imports into separate folders without replacing existing mods.",
            "Combine imported mods?", wxYES_NO | wxCANCEL | wxICON_QUESTION, this);
        if (answer == wxCANCEL) return;
        combineWithExisting = answer == wxYES;
    }

    bool updateOutdated = false;
    if (combineWithExisting && !outdatedMods.empty()) {
        updateOutdated = wxMessageBox(
            "The archive has a higher modversion for " + FromUtf8(std::to_string(outdatedMods.size())) +
            " existing mod(s). Replace those local folders with the imported versions?",
            "Update outdated mods?", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) == wxYES;
    }
    bool replaceSameVersion = false;
    if (combineWithExisting && !sameVersionMods.empty()) {
        replaceSameVersion = wxMessageBox(
            "The archive contains " + FromUtf8(std::to_string(sameVersionMods.size())) +
            " mod(s) with the same modversion as the installed copy. Replace those folders anyway?",
            "Replace same-version mods?", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) == wxYES;
    }
    bool importUnknownSeparately = false;
    if (combineWithExisting && !unknownVersions.empty()) {
        importUnknownSeparately = wxMessageBox(
            "Could not compare modversion for " + FromUtf8(std::to_string(unknownVersions.size())) +
            " matching mod(s). Import them into separate folders rather than replacing the installed copies?",
            "Unknown modversion", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) == wxYES;
    }

    struct Action {
        pzmm::ModEntry source;
        pzmm::fs::path destination;
        bool replace = false;
    };
    std::vector<Action> actions;
    std::set<std::string> reservedPaths;
    const auto pathKey = [](const pzmm::fs::path& path) {
        return LowerFilterText(pzmm::PathToUtf8(path.lexically_normal()));
    };
    const auto uniqueDestination = [&](const pzmm::ModEntry& mod) {
        std::string base = mod.folderName.empty() ? "Imported mod" : mod.folderName;
        auto candidate = plan->userModsRoot / pzmm::fs::u8path(base);
        for (size_t suffix = 2;; ++suffix) {
            std::error_code existsError;
            if (!pzmm::fs::exists(candidate, existsError) && !existsError && !reservedPaths.count(pathKey(candidate))) {
                reservedPaths.insert(pathKey(candidate));
                return candidate;
            }
            candidate = plan->userModsRoot / pzmm::fs::u8path(base + " (imported " + std::to_string(suffix) + ")");
        }
    };
    const auto addNew = [&](size_t index) {
        const auto& source = plan->mods[index];
        actions.push_back({source, uniqueDestination(source), false});
    };
    for (const size_t index : newMods) addNew(index);

    if (combineWithExisting) {
        const auto addReplacement = [&](size_t index) {
            const auto& candidate = candidates[index];
            if (!candidate.installed) return;
            const auto destination = candidate.installed->folderPath;
            if (!reservedPaths.insert(pathKey(destination)).second) return;
            actions.push_back({plan->mods[index], destination, true});
        };
        if (updateOutdated) for (const size_t index : outdatedMods) addReplacement(index);
        if (replaceSameVersion) for (const size_t index : sameVersionMods) addReplacement(index);
        if (importUnknownSeparately) for (const size_t index : unknownVersions) addNew(index);
    } else {
        for (size_t i = 0; i < plan->mods.size(); ++i) {
            if (std::find(newMods.begin(), newMods.end(), i) == newMods.end()) addNew(i);
        }
    }

    if (actions.empty()) {
        wxMessageBox("No import operations were selected. Older or uncomparable versions were left unchanged.",
                     "Nothing to import", wxOK | wxICON_INFORMATION, this);
        return;
    }
    const int finalAnswer = wxMessageBox(
        "Proceed with " + FromUtf8(std::to_string(actions.size())) +
        " import operation(s)? Replaced folders are fully replaced and local-only files inside them will be removed.",
        "Confirm mod import", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
    if (finalAnswer != wxYES) return;

    const auto userRoot = plan->userModsRoot;
    const auto workshopRoot = plan->workshopRoot;
    StartWorker([this, plan, actions = std::move(actions), userRoot, workshopRoot]
                (const std::atomic<bool>& cancel) mutable {
        JobResult result;
        size_t importedCount = 0;
        size_t replacedCount = 0;
        size_t failedCount = 0;
        std::ostringstream failures;
        for (const auto& action : actions) {
            if (cancel.load()) break;
            std::string error;
            const auto progress = [this](const std::string& text) { ReportProgress(text); };
            const bool ok = action.replace
                ? pzmm::ReplaceDirectoryContents(action.source.folderPath, action.destination, error, &cancel, progress)
                : pzmm::CopyDirectoryContents(action.source.folderPath, action.destination, error, &cancel, progress);
            if (ok) {
                if (action.replace) ++replacedCount;
                else ++importedCount;
                if (!error.empty()) failures << "\n" << action.source.name << ": " << error;
            } else {
                ++failedCount;
                failures << "\n" << action.source.name << ": " << error;
            }
        }
        result.success = failedCount == 0;
        result.showMessage = true;
        result.message = "Import complete: " + std::to_string(importedCount) + " added, " +
            std::to_string(replacedCount) + " replaced, " + std::to_string(failedCount) +
            " failed." + failures.str();
        result.scan = pzmm::ScanAll(userRoot, workshopRoot, cancel,
            [this](const std::string& text) { ReportProgress(text); }, MetadataCacheDirectory());
        result.hasScan = true;
        return result;
    });
}

void MainFrame::OnCopySelected(wxCommandEvent&) {
    StartCopyOrUpdate(false);
}

void MainFrame::OnUpdateSelected(wxCommandEvent&) {
    StartCopyOrUpdate(true);
}

void MainFrame::OnUpdateUserSelected(wxCommandEvent&) {
    StartUpdateFromUserSelection();
}

void MainFrame::OnRefresh(wxCommandEvent&) {
    StartScan();
}

void MainFrame::OnListSelection(PageKind kind, wxListEvent& event) {
    m_pendingSelectionKind = kind;
    m_pendingSelectionRow = event.GetIndex();
    m_selectionTimer.StartOnce(100);
}

void MainFrame::OnSelectionSettled(wxTimerEvent&) {
    PageWidgets& page = m_pendingSelectionKind == PageKind::UserMods ? m_userPage : m_workshopPage;
    long row = m_pendingSelectionRow;
    if (row < 0 || !(page.list->GetItemState(row, wxLIST_STATE_SELECTED) & wxLIST_STATE_SELECTED)) {
        row = -1;
        for (long selected = page.list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
             selected >= 0; selected = page.list->GetNextItem(selected, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) {
            row = selected;
        }
    }
    if (row >= 0) {
        UpdateDetails(m_pendingSelectionKind, row);
    } else {
        page.details->ChangeValue("Select a mod to inspect its metadata.");
        SetPoster(page, {});
    }
}

void MainFrame::PopulateAllLists() {
    PopulateList(PageKind::UserMods, m_userPage, m_scanResults.userMods);
    PopulateList(PageKind::WorkshopMods, m_workshopPage, m_scanResults.workshopMods);
}

void MainFrame::PopulateList(PageKind kind, PageWidgets& page, const std::vector<pzmm::ModEntry>& mods) {
    page.visibleIndices.clear();
    page.list->DeleteAllItems();
    page.presetOptions.clear();
    for (size_t column = 0; column < page.columnTitles.size(); ++column) {
        const int index = static_cast<int>(column);
        if (!IsPresetFilterColumn(kind, index)) continue;
        auto options = PresetFilterOptions(kind, index);
        std::set<std::string> existingKeys;
        for (const auto& option : options) existingKeys.insert(option.first);
        page.presetOptions.emplace(index, std::move(options));
        auto filter = page.valueFilters.find(index);
        if (filter != page.valueFilters.end()) {
            for (auto it = filter->second.begin(); it != filter->second.end();) {
                if (!existingKeys.count(*it)) it = filter->second.erase(it);
                else ++it;
            }
            if (filter->second.empty()) page.valueFilters.erase(filter);
        }
    }
    RefreshFilterBar(kind, page);
    RefreshListView(kind, page, mods);

    if (kind == PageKind::UserMods) {
        page.details->ChangeValue("Select a mod to inspect its metadata.\n\n" +
            FromUtf8(std::to_string(mods.size()) + " mod folders scanned.\n") +
            "The default.txt column shows the position in the game's enabled-mod queue.");
    } else {
        page.details->ChangeValue("Select a Workshop mod to inspect its metadata.\n\n" +
            FromUtf8(std::to_string(mods.size()) + " mod folders scanned.\n") +
            "Version directories with mod.info are grouped into one row; common is treated as shared assets.\n" +
            "Use multi-selection to copy or update several mods at once.");
    }
    SetPoster(page, {});
}

void MainFrame::RefreshListView(PageKind kind, PageWidgets& page, const std::vector<pzmm::ModEntry>& mods) {
    std::vector<size_t> indices;
    indices.reserve(mods.size());
    for (size_t i = 0; i < mods.size(); ++i) {
        bool match = true;
        for (const auto& filter : page.filters) {
            if (!ColumnValue(kind, mods[i], filter.first).Lower().Contains(filter.second.Lower())) {
                match = false;
                break;
            }
        }
        if (match) {
            for (const auto& filter : page.valueFilters) {
                if (!MatchesPresetFilter(kind, mods[i], filter.first, filter.second)) {
                    match = false;
                    break;
                }
            }
        }
        if (match) indices.push_back(i);
    }
    const int sortColumn = page.sortColumn;
    const bool ascending = page.sortAscending;
    std::stable_sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
        if (sortColumn == 3) {
            const int leftOrder = mods[a].loadOrder;
            const int rightOrder = mods[b].loadOrder;
            if (leftOrder != rightOrder) return ascending ?
                (leftOrder == 0 ? false : rightOrder == 0 ? true : leftOrder < rightOrder) :
                (leftOrder == 0 ? true : rightOrder == 0 ? false : leftOrder > rightOrder);
            return a < b;
        }
        const int cmp = ColumnValue(kind, mods[a], sortColumn).CmpNoCase(ColumnValue(kind, mods[b], sortColumn));
        if (cmp == 0) return a < b;
        return ascending ? cmp < 0 : cmp > 0;
    });
    page.visibleIndices = indices;

    page.list->Freeze();
    page.list->DeleteAllItems();
    for (size_t rowIndex = 0; rowIndex < indices.size(); ++rowIndex) {
        const size_t sourceIndex = indices[rowIndex];
        const auto& mod = mods[sourceIndex];
        const long row = page.list->InsertItem(static_cast<long>(rowIndex), FromUtf8(mod.name));
        for (size_t col = 1; col < page.columnTitles.size(); ++col)
            page.list->SetItem(row, static_cast<int>(col), ColumnValue(kind, mod, static_cast<int>(col)));
        page.list->SetItemData(row, static_cast<wxUIntPtr>(sourceIndex));
    }
    for (size_t col = 0; col < page.columnTitles.size(); ++col) {
        wxListItem item;
        item.SetMask(wxLIST_MASK_TEXT);
        item.SetText(page.columnTitles[col]);
        page.list->SetColumn(static_cast<int>(col), item);
    }
#ifdef _WIN32
    const HWND header = reinterpret_cast<HWND>(::SendMessageW(
        reinterpret_cast<HWND>(page.list->GetHandle()), LVM_GETHEADER, 0, 0));
    if (header) {
        for (size_t col = 0; col < page.columnTitles.size(); ++col) {
            HDITEMW headerItem{};
            headerItem.mask = HDI_FORMAT;
            if (!::SendMessageW(header, HDM_GETITEMW, static_cast<WPARAM>(col),
                                reinterpret_cast<LPARAM>(&headerItem))) continue;
            headerItem.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
            if (static_cast<int>(col) == page.sortColumn)
                headerItem.fmt |= page.sortAscending ? HDF_SORTUP : HDF_SORTDOWN;
            ::SendMessageW(header, HDM_SETITEMW, static_cast<WPARAM>(col),
                           reinterpret_cast<LPARAM>(&headerItem));
        }
    }
#endif
    page.list->Thaw();
}

void MainFrame::UpdateDetails(PageKind kind, long row) {
    PageWidgets* page = nullptr;
    const std::vector<pzmm::ModEntry>* mods = nullptr;
    if (kind == PageKind::UserMods) {
        page = &m_userPage;
        mods = &m_scanResults.userMods;
    } else {
        page = &m_workshopPage;
        mods = &m_scanResults.workshopMods;
    }
    if (!page || !mods || row < 0) return;
    const wxUIntPtr rawIndex = page->list->GetItemData(row);
    const size_t index = static_cast<size_t>(rawIndex);
    if (index >= mods->size()) return;

    const auto& mod = (*mods)[index];
    page->details->ChangeValue(DescribeMod(mod, kind));
    SetPoster(*page, mod.posterPath);
}

wxString MainFrame::DescribeMod(const pzmm::ModEntry& entry, PageKind kind) const {
    std::ostringstream out;
    out << "Name: " << entry.name << "\n";
    out << "Folder: " << entry.folderName << "\n";
    out << "Mod ID: " << (entry.id.empty() ? "(missing)" : entry.id) << "\n";
    out << "Author: " << (entry.author.empty() ? "(not specified)" : entry.author) << "\n";
    out << "modversion: " << (entry.modVersion.empty() ? "(not specified)" : entry.modVersion) << "\n";
    out << "Version minimum: " << (entry.versionMin.empty() ? "(not specified)" : entry.versionMin) << "\n";
    out << "Version maximum: " << (entry.versionMax.empty() ? "(not specified)" : entry.versionMax) << "\n";
    out << "Requires: " << (entry.require.empty() ? "(none listed)" : entry.require) << "\n";
    if (!entry.javaJarFile.empty()) out << "Java JAR: " << entry.javaJarFile << "\n";
    if (!entry.javaPkgName.empty()) out << "Java package: " << entry.javaPkgName << "\n";
    if (!entry.zbVersionMin.empty()) out << "ZombieBuddy minimum: " << entry.zbVersionMin << "\n";
    if (!entry.workshopItemId.empty()) out << "Workshop item: " << entry.workshopItemId << "\n";
    out << "Enabled queue position: " << (entry.loadOrder > 0 ? std::to_string(entry.loadOrder) : "not listed") << "\n";
    out << "Shared common folder: " << (entry.hasCommonFolder ? "yes" : "no") << "\n";
    out << "Available game versions: " << (entry.gameVersions.empty() ? "(none with mod.info)" : pzmm::Join(entry.gameVersions, ", ")) << "\n";
    out << "Folder path: " << pzmm::PathToUtf8(entry.folderPath) << "\n\n";
    out << "Folder timestamp: " << ToUtf8(FolderModifiedLabel(entry)) << " (top-level directory only)\n";
    out << "Description:\n" << (entry.description.empty() ? "(not specified)" : entry.description) << "\n";

    if (kind == PageKind::WorkshopMods) {
        const auto target = pzmm::ResolveTarget(entry, m_scanResults.userMods, m_scanResults.userModsPath);
        out << "\nLocal status: ";
        if (target.ambiguousId) out << "multiple local folders use this ID; automatic update is disabled\n";
        else if (target.installed && target.matchedEntry) {
            out << "installed as '" << target.matchedEntry->folderName << "'";
            if (!target.matchedEntry->modVersion.empty()) out << " (" << target.matchedEntry->modVersion << ")";
            out << "\n";
        } else out << "not installed\n";
        out << "Copy destination: " << pzmm::PathToUtf8(target.path) << "\n";
    }

    if (!entry.versions.empty()) {
        out << "\nParsed mod.info files:\n";
        for (const auto& version : entry.versions) {
            out << "  [" << version.folderLabel << "] " << pzmm::PathToUtf8(version.infoPath) << "\n";
            for (const auto& field : version.fields) {
                out << "      " << field.first << "=" << field.second << "\n";
            }
        }
    } else {
        out << "\nNo mod.info was found in the root or immediate version folders.\n";
    }
    return FromUtf8(out.str());
}

void MainFrame::SetPoster(PageWidgets& page, const pzmm::fs::path& path) {
    if (path.empty()) {
        page.poster->SetBitmap(wxBitmap(1, 1));
        page.poster->SetMinSize(wxSize(180, 25));
        page.poster->Refresh();
        return;
    }

    wxImage image(PathToWx(path), wxBITMAP_TYPE_ANY);
    if (!image.IsOk()) {
        page.poster->SetBitmap(wxBitmap(1, 1));
        page.poster->SetMinSize(wxSize(180, 25));
        page.poster->Refresh();
        return;
    }
    const int maxWidth = 210;
    const int maxHeight = 210;
    const double scale = std::min(static_cast<double>(maxWidth) / image.GetWidth(),
                                  static_cast<double>(maxHeight) / image.GetHeight());
    const int width = std::max(1, static_cast<int>(image.GetWidth() * scale));
    const int height = std::max(1, static_cast<int>(image.GetHeight() * scale));
    if (width != image.GetWidth() || height != image.GetHeight()) {
        image.Rescale(width, height, wxIMAGE_QUALITY_HIGH);
    }
    page.poster->SetBitmap(wxBitmap(image));
    page.poster->SetMinSize(wxSize(width, height));
    page.poster->Refresh();
}

void MainFrame::SetBusy(bool busy) {
    m_busy = busy;
    m_refreshButton->Enable(!busy);
    m_importButton->Enable(!busy);
    m_userPathCtrl->Enable(!busy);
    m_workshopPathCtrl->Enable(!busy);
    m_browseUserButton->Enable(!busy);
    m_browseWorkshopButton->Enable(!busy);
    m_notebook->Enable(true);
    if (m_copyButton) m_copyButton->Enable(!busy);
    if (m_updateButton) m_updateButton->Enable(!busy);
    if (m_updateUserButton) m_updateUserButton->Enable(!busy);
}

std::vector<size_t> MainFrame::SelectedIndices(const PageWidgets& page) const {
    std::vector<size_t> indices;
    for (long row = page.list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
         row >= 0; row = page.list->GetNextItem(row, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) {
        indices.push_back(static_cast<size_t>(page.list->GetItemData(row)));
    }
    return indices;
}

bool MainFrame::PathsMatchLastScan() const {
    if (!m_hasScan) return false;
    return ToFsPath(m_userPathCtrl->GetValue()).lexically_normal() == m_scanResults.userModsPath.lexically_normal() &&
           ToFsPath(m_workshopPathCtrl->GetValue()).lexically_normal() == m_scanResults.workshopPath.lexically_normal();
}

void MainFrame::ArchiveSelectedMods(PageKind kind) {
    if (m_busy) return;
    if (!m_hasScan || !PathsMatchLastScan()) {
        wxMessageBox("Refresh the scan after changing either folder path before archiving mods.",
                     "Refresh required", wxOK | wxICON_INFORMATION, this);
        return;
    }

    const PageWidgets& page = kind == PageKind::UserMods ? m_userPage : m_workshopPage;
    const auto& entries = kind == PageKind::UserMods ? m_scanResults.userMods : m_scanResults.workshopMods;
    const auto selected = SelectedIndices(page);
    if (selected.empty()) {
        wxMessageBox("Select one or more mods to archive.", "No mods selected", wxOK | wxICON_INFORMATION, this);
        return;
    }

    std::vector<pzmm::ModEntry> mods;
    for (const size_t index : selected) {
        if (index < entries.size()) mods.push_back(entries[index]);
    }
    if (mods.empty()) {
        wxMessageBox("No valid mods were selected.", "No mods selected", wxOK | wxICON_INFORMATION, this);
        return;
    }

    const auto outputFolder = m_scanResults.userModsPath;
    StartWorker([this, mods = std::move(mods), outputFolder]
                (const std::atomic<bool>& cancel) mutable {
        JobResult result;
        std::string error;
        result.success = CreateModArchive(mods, outputFolder, cancel,
            [this](const std::string& text) { ReportProgress(text); }, result.archivePath, error);
        result.showArchiveNotification = result.success;
        result.showMessage = !result.success;
        result.message = result.success
            ? "ZIP archive created: " + pzmm::PathToUtf8(result.archivePath)
            : "Could not create ZIP archive. " + error;
        return result;
    });
}

void MainFrame::ShowArchiveNotification(const pzmm::fs::path& archivePath) {
    wxDialog dialog(this, wxID_ANY, "Mod archive created", wxDefaultPosition, wxDefaultSize,
                    wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* outerSizer = new wxBoxSizer(wxVERTICAL);
    outerSizer->Add(new wxStaticText(&dialog, wxID_ANY, "The ZIP archive was saved to:"),
                    0, wxLEFT | wxRIGHT | wxTOP, 12);
    outerSizer->Add(new wxStaticText(&dialog, wxID_ANY, PathToWx(archivePath.parent_path())),
                    0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    auto* archivePathCtrl = new wxTextCtrl(&dialog, wxID_ANY, PathToWx(archivePath),
        wxDefaultPosition, wxSize(520, -1), wxTE_READONLY | wxTE_DONTWRAP);
    outerSizer->Add(archivePathCtrl, 0, wxEXPAND | wxALL, 12);

    auto* buttonSizer = new wxBoxSizer(wxHORIZONTAL);
    auto* openButton = new wxButton(&dialog, wxID_OPEN, "Show ZIP in Explorer");
    openButton->SetBitmap(wxArtProvider::GetBitmap(wxART_FILE_OPEN, wxART_BUTTON, wxSize(16, 16)));
    auto* closeButton = new wxButton(&dialog, wxID_CLOSE, "Close");
    buttonSizer->Add(openButton, 0, wxRIGHT, 8);
    buttonSizer->Add(closeButton, 0);
    outerSizer->Add(buttonSizer, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    dialog.SetSizerAndFit(outerSizer);
    dialog.CentreOnParent();
    openButton->Bind(wxEVT_BUTTON, [&dialog, archivePath](wxCommandEvent&) {
        if (!OpenInExplorer(archivePath, true))
            wxMessageBox("Could not open the archive location.", "Open failed", wxOK | wxICON_WARNING, &dialog);
    });
    closeButton->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_CLOSE); });
    dialog.ShowModal();
}

void MainFrame::StartCopyOrUpdate(bool updateOnly) {
    if (m_busy) return;
    if (!m_hasScan || !PathsMatchLastScan()) {
        wxMessageBox("Refresh the scan after changing either folder path before copying or updating.",
                     "Refresh required", wxOK | wxICON_INFORMATION, this);
        return;
    }
    const auto selected = SelectedIndices(m_workshopPage);
    if (selected.empty()) {
        wxMessageBox("Select one or more mods in the Steam Workshop list first.",
                     "No mods selected", wxOK | wxICON_INFORMATION, this);
        return;
    }

    const auto userRoot = ToFsPath(m_userPathCtrl->GetValue());
    const auto workshopRoot = ToFsPath(m_workshopPathCtrl->GetValue());
    std::vector<CopyJob> jobs;
    bool existingDestination = false;
    bool ambiguousDestination = false;
    for (const size_t index : selected) {
        if (index >= m_scanResults.workshopMods.size()) continue;
        const auto& source = m_scanResults.workshopMods[index];
        const auto target = pzmm::ResolveTarget(source, m_scanResults.userMods, userRoot);
        CopyJob job;
        job.source = source;
        if (target.matchedEntry) job.installed = *target.matchedEntry;
        job.targetPath = target.path;
        if (!updateOnly && target.ambiguousId) {
            job.targetPath = userRoot / source.folderPath.filename();
            job.installed.reset();
            ambiguousDestination = true;
        }
        std::error_code existsError;
        existingDestination = existingDestination ||
            (pzmm::fs::exists(job.targetPath, existsError) && !existsError);
        jobs.push_back(std::move(job));
    }
    if (jobs.empty()) {
        wxMessageBox("No valid Workshop mods were selected.", "No mods selected", wxOK | wxICON_INFORMATION, this);
        return;
    }

    if (!updateOnly && (existingDestination || ambiguousDestination)) {
        wxString prompt = "Copy/merge " + FromUtf8(std::to_string(jobs.size())) +
            " selected mod(s) into the user mods folder?\n\nMatching files may be overwritten; extra local files are preserved.";
        if (ambiguousDestination) {
            prompt += "\n\nSome IDs are ambiguous and will be copied to separate folder names rather than updating an arbitrary match.";
        }
        if (wxMessageBox(prompt, "Confirm batch copy", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) return;
    }

    const auto method = (m_updateMethodChoice && m_updateMethodChoice->GetSelection() == 1)
        ? pzmm::UpdateMethod::FileModificationTime
        : pzmm::UpdateMethod::ModInfoVersion;

    if (updateOnly) {
        const wxString prompt = "Update " + FromUtf8(std::to_string(jobs.size())) +
            " selected mod(s) when newer Workshop files are found? Updated folders will be replaced completely; "
            "local-only files in those folders will be removed.";
        if (wxMessageBox(prompt, "Confirm full mod replacement", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES)
            return;
    }

    RunCopyJobs(std::move(jobs), updateOnly, method, userRoot, workshopRoot);
}

void MainFrame::StartUpdateFromUserSelection() {
    if (m_busy) return;
    if (!m_hasScan || !PathsMatchLastScan()) {
        wxMessageBox("Refresh the scan after changing either folder path before updating mods.",
                     "Refresh required", wxOK | wxICON_INFORMATION, this);
        return;
    }
    const auto selected = SelectedIndices(m_userPage);
    if (selected.empty()) {
        wxMessageBox("Select one or more local mods first.", "No mods selected", wxOK | wxICON_INFORMATION, this);
        return;
    }

    const auto userRoot = ToFsPath(m_userPathCtrl->GetValue());
    const auto workshopRoot = ToFsPath(m_workshopPathCtrl->GetValue());
    std::vector<CopyJob> jobs;
    for (const size_t index : selected) {
        if (index >= m_scanResults.userMods.size()) continue;
        const auto& local = m_scanResults.userMods[index];
        for (const auto& source : m_scanResults.workshopMods) {
            const bool folderMatch = pzmm::NormalizeId(source.folderName) == pzmm::NormalizeId(local.folderName);
            const bool idMatch = !source.localMatchAmbiguous && !local.id.empty() &&
                pzmm::NormalizeId(source.id) == pzmm::NormalizeId(local.id);
            if (!folderMatch && !idMatch) continue;
            CopyJob job;
            job.source = source;
            job.installed = local;
            job.targetPath = local.folderPath;
            jobs.push_back(std::move(job));
            break;
        }
    }
    if (jobs.empty()) {
        wxMessageBox("No selected local mods have an unambiguous Workshop counterpart.",
                     "No Workshop match", wxOK | wxICON_INFORMATION, this);
        return;
    }
    const wxString prompt = "Check " + FromUtf8(std::to_string(jobs.size())) +
        " selected local mod(s) for a newer Workshop version? Updated folders will be replaced completely; "
        "local-only files in those folders will be removed.";
    if (wxMessageBox(prompt, "Confirm full mod replacement", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES)
        return;
    const auto method = m_userUpdateMethodChoice && m_userUpdateMethodChoice->GetSelection() == 1
        ? pzmm::UpdateMethod::FileModificationTime
        : pzmm::UpdateMethod::ModInfoVersion;
    RunCopyJobs(std::move(jobs), true, method, userRoot, workshopRoot);
}

void MainFrame::RunCopyJobs(std::vector<CopyJob> jobs, bool updateOnly, pzmm::UpdateMethod method,
                            const pzmm::fs::path& userRoot, const pzmm::fs::path& workshopRoot) {
    StartWorker([this, jobs = std::move(jobs), userRoot, workshopRoot, updateOnly, method]
                (const std::atomic<bool>& cancel) mutable {
        JobResult result;
        size_t completed = 0;
        size_t skipped = 0;
        size_t failed = 0;
        std::ostringstream summary;
        for (const auto& job : jobs) {
            if (cancel.load()) break;
            std::string operationReason;
            bool copyNeeded = !updateOnly;
            if (updateOnly) {
                if (!job.installed) {
                    ++skipped;
                    summary << "\n" << job.source.name << ": not installed or ambiguous; skipped.";
                    continue;
                }
                copyNeeded = pzmm::IsUpdateAvailable(job.source, *job.installed, method, cancel, operationReason);
                if (!copyNeeded) {
                    ++skipped;
                    summary << "\n" << job.source.name << ": " << operationReason;
                    continue;
                }
            }
            std::string copyError;
            const auto reportProgress = [this](const std::string& text) { ReportProgress(text); };
            const bool copied = updateOnly
                ? pzmm::ReplaceDirectoryContents(job.source.folderPath, job.targetPath, copyError, &cancel, reportProgress)
                : pzmm::CopyDirectoryContents(job.source.folderPath, job.targetPath, copyError, &cancel, reportProgress);
            if (copied) {
                ++completed;
                if (!copyError.empty()) summary << "\n" << job.source.name << ": " << copyError;
            } else {
                ++failed;
                summary << "\n" << job.source.name << ": " << copyError;
            }
        }
        result.success = failed == 0;
        result.message = (updateOnly ? "Batch update complete: " : "Batch copy complete: ") +
            std::to_string(completed) + " copied/updated, " + std::to_string(skipped) +
            " skipped, " + std::to_string(failed) + " failed." + summary.str();
        result.showMessage = true;
        result.scan = pzmm::ScanAll(userRoot, workshopRoot, cancel,
            [this](const std::string& text) { ReportProgress(text); }, MetadataCacheDirectory());
        result.hasScan = true;
        return result;
    });
}

void MainFrame::DeleteSelectedUserMods() {
    if (m_busy) return;
    if (!m_hasScan || !PathsMatchLastScan()) {
        wxMessageBox("Refresh the scan after changing the folder path before deleting mods.",
                     "Refresh required", wxOK | wxICON_INFORMATION, this);
        return;
    }
    const auto selected = SelectedIndices(m_userPage);
    if (selected.empty()) {
        wxMessageBox("Select one or more user mods first.", "No mods selected", wxOK | wxICON_INFORMATION, this);
        return;
    }

    const auto userRoot = ToFsPath(m_userPathCtrl->GetValue()).lexically_normal();
    std::vector<pzmm::fs::path> paths;
    for (const size_t index : selected) {
        if (index >= m_scanResults.userMods.size()) continue;
        const auto path = m_scanResults.userMods[index].folderPath.lexically_normal();
        if (path.parent_path() == userRoot) paths.push_back(path);
    }
    if (paths.empty()) {
        wxMessageBox("The selected paths are not direct children of the configured user mods folder.",
                     "Delete refused", wxOK | wxICON_WARNING, this);
        return;
    }
    const wxString prompt = "Permanently delete " + FromUtf8(std::to_string(paths.size())) +
        " selected mod folder(s) and all their contents? This cannot be undone.";
    if (wxMessageBox(prompt, "Delete user mods", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES) return;

    const auto workshopRoot = ToFsPath(m_workshopPathCtrl->GetValue());
    StartWorker([this, paths = std::move(paths), userRoot, workshopRoot]
                (const std::atomic<bool>& cancel) mutable {
        JobResult result;
        size_t deleted = 0;
        size_t failed = 0;
        std::ostringstream errors;
        for (const auto& path : paths) {
            if (cancel.load()) break;
            std::error_code error;
            pzmm::fs::remove_all(path, error);
            if (error) {
                ++failed;
                errors << "\n" << pzmm::PathToUtf8(path.filename()) << ": " << error.message();
            } else {
                ++deleted;
            }
        }
        result.success = failed == 0;
        result.message = "Deleted " + std::to_string(deleted) + " mod folder(s); " +
            std::to_string(failed) + " failed." + errors.str();
        result.showMessage = true;
        result.scan = pzmm::ScanAll(userRoot, workshopRoot, cancel,
            [this](const std::string& text) { ReportProgress(text); });
        result.hasScan = true;
        return result;
    });
}
