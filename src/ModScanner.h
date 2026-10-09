#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pzmm {
namespace fs = std::filesystem;

struct ModVersionInfo {
    std::string folderLabel;       // e.g. "42.21.0" or "root"
    fs::path folderPath;
    fs::path infoPath;
    fs::path posterPath;
    std::map<std::string, std::string> fields;
};

struct ModEntry {
    std::string folderName;
    std::string name;
    std::string id;
    std::string description;
    std::string author;
    std::string modVersion;
    std::string versionMin;
    std::string versionMax;
    std::string require;
    std::string javaJarFile;
    std::string javaPkgName;
    std::string zbVersionMin;
    std::string workshopItemId;

    fs::path folderPath;
    fs::file_time_type folderModifiedTime = fs::file_time_type::min();
    fs::path posterPath;
    std::map<std::string, std::string> metadata;
    std::vector<ModVersionInfo> versions;
    std::vector<std::string> gameVersions;
    bool hasCommonFolder = false;
    bool workshopCounterpartFound = false;
    bool workshopUpdateAvailable = false;
    bool locallyInstalled = false;
    bool localMatchAmbiguous = false;
    int loadOrder = 0; // 1-based position in default.txt; 0 means absent.
};

struct ScanResults {
    fs::path userModsPath;
    fs::path workshopPath;
    fs::path defaultTxtPath;
    fs::path customTxtPath;
    bool defaultTxtFound = false;
    bool customTxtWritten = false;
    std::string customTxtWarning;
    std::vector<std::string> defaultModIds;
    std::vector<ModEntry> userMods;
    std::vector<ModEntry> workshopMods;
};

enum class UpdateMethod {
    ModInfoVersion,
    FileModificationTime
};

struct TargetResolution {
    fs::path path;
    bool installed = false;
    bool ambiguousId = false;
    const ModEntry* matchedEntry = nullptr;
};

std::string PathToUtf8(const fs::path& path);
std::string GetField(const std::map<std::string, std::string>& fields, const std::string& key);
std::string NormalizeId(const std::string& value);
std::string Join(const std::vector<std::string>& values, const std::string& separator);

using ProgressCallback = std::function<void(const std::string&)>;

ScanResults ScanAll(const fs::path& userModsPath,
                    const fs::path& workshopPath,
                    const std::atomic<bool>& cancel,
                    ProgressCallback progress = {},
                    const fs::path& metadataCacheDirectory = {});

TargetResolution ResolveTarget(const ModEntry& workshopMod,
                               const std::vector<ModEntry>& userMods,
                               const fs::path& userModsPath);

bool IsUpdateAvailable(const ModEntry& source,
                       const ModEntry& installed,
                       UpdateMethod method,
                       const std::atomic<bool>& cancel,
                       std::string& reason);

bool CopyDirectoryContents(const fs::path& source,
                           const fs::path& destination,
                           std::string& errorMessage,
                           const std::atomic<bool>* cancel = nullptr,
                           ProgressCallback progress = {});

bool ReplaceDirectoryContents(const fs::path& source,
                              const fs::path& destination,
                              std::string& errorMessage,
                              const std::atomic<bool>* cancel = nullptr,
                              ProgressCallback progress = {});

std::string LoadStatus(const ModEntry& entry);

} // namespace pzmm
