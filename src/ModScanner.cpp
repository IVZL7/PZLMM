#include "ModScanner.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>

namespace pzmm {
namespace {

std::string Trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool IsDirectory(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec) && !ec;
}

bool IsRegularFile(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec) && !ec;
}

std::map<std::string, std::string> ReadModInfo(const fs::path& infoPath,
                                                const std::atomic<bool>& cancel) {
    std::map<std::string, std::string> fields;
    std::ifstream input(infoPath, std::ios::binary);
    if (!input) return fields;

    std::string line;
    bool firstLine = true;
    while (!cancel.load() && std::getline(input, line)) {
        if (firstLine && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3); // UTF-8 BOM
        }
        firstLine = false;
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = Lower(Trim(line.substr(0, equals)));
        const std::string value = Trim(line.substr(equals + 1));
        if (!key.empty()) fields[key] = value;
    }
    return fields;
}

bool VersionLabelLess(const std::string& left, const std::string& right) {
    // Compare numeric components, so "42.9" sorts before "42.10".
    static const std::regex numberPattern(R"((\d+))");
    std::vector<unsigned long long> a;
    std::vector<unsigned long long> b;
    for (std::sregex_iterator it(left.begin(), left.end(), numberPattern), end; it != end; ++it) {
        try { a.push_back(std::stoull((*it)[1].str())); } catch (...) { a.push_back(0); }
    }
    for (std::sregex_iterator it(right.begin(), right.end(), numberPattern), end; it != end; ++it) {
        try { b.push_back(std::stoull((*it)[1].str())); } catch (...) { b.push_back(0); }
    }
    if (a.empty() || b.empty()) return Lower(left) < Lower(right);
    const size_t count = std::max(a.size(), b.size());
    for (size_t i = 0; i < count; ++i) {
        const auto av = i < a.size() ? a[i] : 0ULL;
        const auto bv = i < b.size() ? b[i] : 0ULL;
        if (av != bv) return av < bv;
    }
    // Numeric versions such as 1.2 and 1.2.0 are considered equivalent.
    return false;
}

int CompareVersionStrings(const std::string& left, const std::string& right) {
    if (Trim(left).empty() || Trim(right).empty()) return 0;
    if (VersionLabelLess(left, right)) return -1;
    if (VersionLabelLess(right, left)) return 1;
    return 0;
}

std::optional<std::string> FindPoster(const fs::path& versionFolder,
                                      const fs::path& modRoot,
                                      const std::map<std::string, std::string>& fields) {
    const std::string posterName = GetField(fields, "poster");
    if (!posterName.empty()) {
        fs::path requested = versionFolder / fs::path(posterName);
        if (IsRegularFile(requested)) return PathToUtf8(requested);
        requested = modRoot / fs::path(posterName);
        if (IsRegularFile(requested)) return PathToUtf8(requested);
    }
    const fs::path localPoster = versionFolder / "poster.png";
    if (IsRegularFile(localPoster)) return PathToUtf8(localPoster);
    const fs::path rootPoster = modRoot / "poster.png";
    if (IsRegularFile(rootPoster)) return PathToUtf8(rootPoster);
    return std::nullopt;
}

ModVersionInfo MakeVersionInfo(const std::string& label,
                               const fs::path& folder,
                               const fs::path& infoPath,
                               const fs::path& modRoot,
                               const std::atomic<bool>& cancel) {
    ModVersionInfo version;
    version.folderLabel = label;
    version.folderPath = folder;
    version.infoPath = infoPath;
    version.fields = ReadModInfo(infoPath, cancel);
    const auto poster = FindPoster(folder, modRoot, version.fields);
    if (poster) version.posterPath = fs::u8path(*poster);
    return version;
}

ModEntry ScanModRoot(const fs::path& modRoot,
                     const std::string& workshopItemId,
                     const std::atomic<bool>& cancel) {
    ModEntry entry;
    entry.folderPath = modRoot;
    entry.folderName = PathToUtf8(modRoot.filename());
    entry.workshopItemId = workshopItemId;
    entry.name = entry.folderName;
    entry.hasCommonFolder = IsDirectory(modRoot / "common");

    // Legacy layouts may have mod.info directly in the mod root.
    const fs::path rootInfo = modRoot / "mod.info";
    if (IsRegularFile(rootInfo) && !cancel.load()) {
        entry.versions.push_back(MakeVersionInfo("root", modRoot, rootInfo, modRoot, cancel));
    }

    const fs::path commonFolder = modRoot / "common";
    const fs::path commonInfo = commonFolder / "mod.info";
    if (IsRegularFile(commonInfo) && !cancel.load()) {
        entry.versions.push_back(MakeVersionInfo("common", commonFolder, commonInfo, modRoot, cancel));
    }

    std::error_code ec;
    fs::directory_iterator it(modRoot, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end && !cancel.load(); it.increment(ec)) {
        std::error_code typeError;
        if (!it->is_directory(typeError) || typeError) continue;
        const std::string childName = PathToUtf8(it->path().filename());
        if (Lower(childName) == "common") continue;
        const fs::path info = it->path() / "mod.info";
        if (!IsRegularFile(info)) continue;
        entry.versions.push_back(MakeVersionInfo(childName, it->path(), info, modRoot, cancel));
    }

    // Root metadata is a fallback; versioned metadata is preferred, with the highest
    // game-version folder supplying the main display fields.
    std::stable_sort(entry.versions.begin(), entry.versions.end(), [](const auto& a, const auto& b) {
        const bool aIsRoot = a.folderLabel == "root";
        const bool bIsRoot = b.folderLabel == "root";
        const bool aIsCommon = a.folderLabel == "common";
        const bool bIsCommon = b.folderLabel == "common";
        if (aIsRoot != bIsRoot) return aIsRoot;
        if (aIsCommon != bIsCommon) return !aIsCommon;
        if (aIsRoot || aIsCommon) return false;
        return VersionLabelLess(a.folderLabel, b.folderLabel);
    });

    const ModVersionInfo* preferred = nullptr;
    for (const auto& version : entry.versions) {
        if (version.folderLabel != "root" && version.folderLabel != "common") preferred = &version;
    }
    if (!preferred) {
        for (const auto& version : entry.versions) {
            if (version.folderLabel == "root") {
                preferred = &version;
                break;
            }
        }
    }
    if (!preferred) {
        for (const auto& version : entry.versions) {
            if (version.folderLabel == "common") {
                preferred = &version;
                break;
            }
        }
    }

    if (preferred) {
        entry.metadata = preferred->fields;
        entry.id = GetField(entry.metadata, "id");
        entry.name = GetField(entry.metadata, "name");
        if (entry.name.empty()) entry.name = entry.folderName;
        entry.description = GetField(entry.metadata, "description");
        entry.author = GetField(entry.metadata, "author");
        entry.modVersion = GetField(entry.metadata, "modversion");
        entry.versionMin = GetField(entry.metadata, "versionmin");
        entry.versionMax = GetField(entry.metadata, "versionmax");
        entry.require = GetField(entry.metadata, "require");
        entry.javaJarFile = GetField(entry.metadata, "javajarfile");
        entry.javaPkgName = GetField(entry.metadata, "javapkgname");
        entry.zbVersionMin = GetField(entry.metadata, "zbversionmin");
        entry.posterPath = preferred->posterPath;
    }

    // Collect only directories that actually contain mod.info. "common" is a shared
    // asset directory and is deliberately not represented as a separate version.
    for (const auto& version : entry.versions) {
        if (version.folderLabel != "root" && version.folderLabel != "common")
            entry.gameVersions.push_back(version.folderLabel);
        if (entry.posterPath.empty() && !version.posterPath.empty()) entry.posterPath = version.posterPath;
    }
    if (entry.posterPath.empty()) {
        const fs::path commonPoster = modRoot / "common" / "poster.png";
        if (IsRegularFile(commonPoster)) entry.posterPath = commonPoster;
    }

    return entry;
}

std::vector<std::string> ReadDefaultModOrder(const fs::path& filePath, bool& found) {
    std::vector<std::string> ids;
    found = IsRegularFile(filePath);
    if (!found) return ids;

    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        found = false;
        return ids;
    }

    bool inModsBlock = false;
    bool waitingForModsBrace = false;
    std::string line;
    const std::regex modLine(R"(^\s*mod\s*=\s*(.*?)\s*,\s*$)", std::regex::icase);
    while (std::getline(input, line)) {
        const std::string trimmed = Trim(line);
        const std::string lower = Lower(trimmed);
        if (!inModsBlock) {
            if (waitingForModsBrace) {
                if (trimmed.find('{') != std::string::npos) inModsBlock = true;
                waitingForModsBrace = false;
                continue;
            }
            if (lower == "mods") {
                waitingForModsBrace = true;
                continue;
            }
            if (lower.rfind("mods", 0) == 0 && trimmed.find('{') != std::string::npos) {
                inModsBlock = true;
                continue;
            }
        }
        if (inModsBlock && trimmed.find('}') != std::string::npos) {
            inModsBlock = false;
            continue;
        }
        if (!inModsBlock) continue;
        std::smatch match;
        if (std::regex_match(line, match, modLine)) {
            const std::string id = Trim(match[1].str());
            if (!id.empty()) ids.push_back(id);
        }
    }
    return ids;
}

void ApplyLoadOrder(std::vector<ModEntry>& mods,
                    const std::vector<std::string>& order) {
    std::map<std::string, int> positions;
    for (size_t i = 0; i < order.size(); ++i) {
        // First occurrence wins if a malformed default.txt repeats an ID.
        positions.emplace(NormalizeId(order[i]), static_cast<int>(i + 1));
    }
    for (auto& mod : mods) {
        const auto it = positions.find(NormalizeId(mod.id));
        mod.loadOrder = it == positions.end() ? 0 : it->second;
    }
}

std::vector<ModEntry> ScanUserMods(const fs::path& root,
                                   const std::atomic<bool>& cancel,
                                   const ProgressCallback& progress) {
    std::vector<ModEntry> mods;
    if (!IsDirectory(root)) return mods;

    std::error_code ec;
    fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end && !cancel.load(); it.increment(ec)) {
        std::error_code typeError;
        if (!it->is_directory(typeError) || typeError) continue;
        if (progress) progress("Reading user mod: " + PathToUtf8(it->path().filename()));
        mods.push_back(ScanModRoot(it->path(), {}, cancel));
    }
    std::sort(mods.begin(), mods.end(), [](const auto& a, const auto& b) {
        const auto an = Lower(a.name);
        const auto bn = Lower(b.name);
        if (an != bn) return an < bn;
        return Lower(a.id) < Lower(b.id);
    });
    return mods;
}

std::vector<ModEntry> ScanWorkshopMods(const fs::path& root,
                                       const std::atomic<bool>& cancel,
                                       const ProgressCallback& progress) {
    std::vector<ModEntry> mods;
    if (!IsDirectory(root)) return mods;

    std::error_code workshopError;
    fs::directory_iterator workshopIt(root, fs::directory_options::skip_permission_denied, workshopError), end;
    for (; !workshopError && workshopIt != end && !cancel.load(); workshopIt.increment(workshopError)) {
        std::error_code typeError;
        if (!workshopIt->is_directory(typeError) || typeError) continue;
        const std::string workshopId = PathToUtf8(workshopIt->path().filename());

        // Standard downloaded layout: <WorkshopID>/mods/<internal-mod-folder>.
        // Some packaging tools also produce Contents/mods, so accept that as a fallback.
        fs::path modsDir = workshopIt->path() / "mods";
        if (!IsDirectory(modsDir)) modsDir = workshopIt->path() / "Contents" / "mods";
        if (!IsDirectory(modsDir)) continue;

        std::error_code modsError;
        fs::directory_iterator modIt(modsDir, fs::directory_options::skip_permission_denied, modsError), modEnd;
        for (; !modsError && modIt != modEnd && !cancel.load(); modIt.increment(modsError)) {
            std::error_code childTypeError;
            if (!modIt->is_directory(childTypeError) || childTypeError) continue;
            if (progress) progress("Reading Workshop mod " + workshopId + ": " + PathToUtf8(modIt->path().filename()));
            mods.push_back(ScanModRoot(modIt->path(), workshopId, cancel));
        }
    }

    std::sort(mods.begin(), mods.end(), [](const auto& a, const auto& b) {
        const auto an = Lower(a.name);
        const auto bn = Lower(b.name);
        if (an != bn) return an < bn;
        if (Lower(a.id) != Lower(b.id)) return Lower(a.id) < Lower(b.id);
        return a.workshopItemId < b.workshopItemId;
    });
    return mods;
}


std::string SingleLine(std::string value) {
    for (char& ch : value) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    }
    return value;
}

bool WriteCustomInventory(const fs::path& userModsPath,
                          const std::vector<ModEntry>& userMods,
                          std::string& warning) {
    if (!IsDirectory(userModsPath)) {
        warning = "User mods folder does not exist; custom.txt was not written.";
        return false;
    }

    const fs::path outputPath = userModsPath / "custom.txt";
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        warning = "Could not write " + PathToUtf8(outputPath) + ". Check folder permissions.";
        return false;
    }

    output << "# Project Zomboid Mod Manager inventory v1\n";
    output << "# Generated from the immediate folders inside this user mods directory.\n";
    output << "# This is an inventory snapshot, not the game's enabled-mod config.\n";
    output << "count=" << userMods.size() << "\n\n";
    for (const auto& mod : userMods) {
        output << "[mod]\n";
        output << "folder=" << SingleLine(mod.folderName) << "\n";
        output << "name=" << SingleLine(mod.name) << "\n";
        output << "id=" << SingleLine(mod.id) << "\n";
        output << "modversion=" << SingleLine(mod.modVersion) << "\n";
        output << "gameVersions=" << SingleLine(Join(mod.gameVersions, ", ")) << "\n";
        output << "enabledOrder=" << mod.loadOrder << "\n";
        output << "path=" << SingleLine(PathToUtf8(mod.folderPath)) << "\n\n";
    }
    output.flush();
    if (!output) {
        warning = "An error occurred while writing " + PathToUtf8(outputPath) + ".";
        return false;
    }
    return true;
}

std::optional<fs::file_time_type> NewestWriteTime(const fs::path& root,
                                                  const std::atomic<bool>& cancel) {
    std::optional<fs::file_time_type> newest;
    if (!IsDirectory(root)) return newest;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end && !cancel.load(); it.increment(ec)) {
        std::error_code fileError;
        if (!it->is_regular_file(fileError) || fileError) continue;
        const auto time = fs::last_write_time(it->path(), fileError);
        if (fileError) continue;
        if (!newest || time > *newest) newest = time;
    }
    return newest;
}

std::set<std::string> VersionSet(const ModEntry& entry) {
    std::set<std::string> versions;
    for (const auto& version : entry.gameVersions) versions.insert(Lower(version));
    return versions;
}

} // namespace

std::string PathToUtf8(const fs::path& path) {
#if defined(_WIN32)
    const auto value = path.u8string();
    return std::string(value.begin(), value.end());
#else
    return path.string();
#endif
}

std::string GetField(const std::map<std::string, std::string>& fields, const std::string& key) {
    const auto it = fields.find(Lower(key));
    return it == fields.end() ? std::string{} : it->second;
}

std::string NormalizeId(const std::string& value) {
    return Lower(Trim(value));
}

std::string Join(const std::vector<std::string>& values, const std::string& separator) {
    std::ostringstream out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out << separator;
        out << values[i];
    }
    return out.str();
}

ScanResults ScanAll(const fs::path& userModsPath,
                    const fs::path& workshopPath,
                    const std::atomic<bool>& cancel,
                    ProgressCallback progress) {
    ScanResults results;
    results.userModsPath = userModsPath;
    results.workshopPath = workshopPath;
    results.defaultTxtPath = userModsPath / "default.txt";
    results.customTxtPath = userModsPath / "custom.txt";
    if (progress) progress("Reading default.txt load order…");
    results.defaultModIds = ReadDefaultModOrder(results.defaultTxtPath, results.defaultTxtFound);
    if (progress) progress("Scanning user mods folder…");
    results.userMods = ScanUserMods(userModsPath, cancel, progress);
    if (progress) progress("Scanning Steam Workshop folder…");
    results.workshopMods = ScanWorkshopMods(workshopPath, cancel, progress);
    ApplyLoadOrder(results.userMods, results.defaultModIds);
    ApplyLoadOrder(results.workshopMods, results.defaultModIds);
    if (!cancel.load()) {
        if (progress) progress("Writing custom.txt inventory…");
        results.customTxtWritten = WriteCustomInventory(userModsPath, results.userMods, results.customTxtWarning);
    }
    return results;
}

TargetResolution ResolveTarget(const ModEntry& workshopMod,
                               const std::vector<ModEntry>& userMods,
                               const fs::path& userModsPath) {
    TargetResolution result;
    result.path = userModsPath / workshopMod.folderPath.filename();

    // Folder-name match is the least ambiguous and is the usual layout.
    for (const auto& local : userMods) {
        if (NormalizeId(local.folderName) == NormalizeId(workshopMod.folderName)) {
            result.path = local.folderPath;
            result.installed = true;
            result.matchedEntry = &local;
            return result;
        }
    }

    if (!workshopMod.id.empty()) {
        std::vector<const ModEntry*> idMatches;
        for (const auto& local : userMods) {
            if (!local.id.empty() && NormalizeId(local.id) == NormalizeId(workshopMod.id)) {
                idMatches.push_back(&local);
            }
        }
        if (idMatches.size() == 1) {
            result.path = idMatches.front()->folderPath;
            result.installed = true;
            result.matchedEntry = idMatches.front();
        } else if (idMatches.size() > 1) {
            result.ambiguousId = true;
        }
    }
    return result;
}

bool IsUpdateAvailable(const ModEntry& source,
                       const ModEntry& installed,
                       UpdateMethod method,
                       const std::atomic<bool>& cancel,
                       std::string& reason) {
    if (method == UpdateMethod::FileModificationTime) {
        const auto sourceTime = NewestWriteTime(source.folderPath, cancel);
        const auto installedTime = NewestWriteTime(installed.folderPath, cancel);
        if (cancel.load()) {
            reason = "Comparison cancelled.";
            return false;
        }
        if (!sourceTime || !installedTime) {
            reason = "Could not determine modification times for every folder.";
            return false;
        }
        if (*sourceTime > *installedTime) {
            reason = "Workshop files have a newer modification time.";
            return true;
        }
        reason = "The newest file timestamp in the user folder is equal or newer.";
        return false;
    }

    const int modVersionComparison = CompareVersionStrings(source.modVersion, installed.modVersion);
    if (!source.modVersion.empty() && !installed.modVersion.empty() && modVersionComparison != 0) {
        if (modVersionComparison > 0) {
            reason = "Workshop modversion " + source.modVersion + " is newer than " + installed.modVersion + ".";
            return true;
        }
        reason = "Installed modversion " + installed.modVersion + " is newer than " + source.modVersion + ".";
        return false;
    }

    // Some mods omit modversion. Compare the available game-version folder names and
    // prefer a Workshop package that includes a game version missing from the local copy.
    const auto sourceVersions = VersionSet(source);
    const auto installedVersions = VersionSet(installed);
    for (const auto& version : sourceVersions) {
        if (!installedVersions.count(version)) {
            reason = "Workshop contains game-version folder " + version + " not present locally.";
            return true;
        }
    }

    if (source.modVersion.empty() || installed.modVersion.empty()) {
        std::string sourceLatest;
        std::string installedLatest;
        for (const auto& version : source.gameVersions) {
            if (sourceLatest.empty() || VersionLabelLess(sourceLatest, version)) sourceLatest = version;
        }
        for (const auto& version : installed.gameVersions) {
            if (installedLatest.empty() || VersionLabelLess(installedLatest, version)) installedLatest = version;
        }
        const int gameVersionComparison = CompareVersionStrings(sourceLatest, installedLatest);
        if (gameVersionComparison > 0) {
            reason = "Workshop has a newer version folder (" + sourceLatest + ").";
            return true;
        }
    }

    reason = "mod.info versions and game-version folders do not indicate a newer Workshop copy.";
    return false;
}

bool CopyDirectoryContents(const fs::path& source,
                           const fs::path& destination,
                           std::string& errorMessage,
                           const std::atomic<bool>* cancel,
                           ProgressCallback progress) {
    if (!IsDirectory(source)) {
        errorMessage = "Workshop mod folder no longer exists: " + PathToUtf8(source);
        return false;
    }
    if (source == destination || destination.lexically_normal().string().find(source.lexically_normal().string()) == 0) {
        errorMessage = "Source and destination paths overlap; refusing to copy.";
        return false;
    }

    // Count files up front so the copy/update operation can report a determinate percentage.
    size_t totalFiles = 0;
    {
        std::error_code countError;
        fs::recursive_directory_iterator countIt(source, fs::directory_options::skip_permission_denied, countError), countEnd;
        for (; !countError && countIt != countEnd; countIt.increment(countError)) {
            if (cancel && cancel->load()) { errorMessage = "Operation cancelled."; return false; }
            std::error_code typeError;
            if (countIt->is_regular_file(typeError) && !typeError) ++totalFiles;
        }
    }
    size_t copiedFiles = 0;

    std::error_code ec;
    fs::create_directories(destination, ec);
    if (ec) {
        errorMessage = "Could not create destination folder: " + ec.message();
        return false;
    }

    fs::recursive_directory_iterator it(source, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) {
        errorMessage = "Could not read the Workshop source folder: " + ec.message();
        return false;
    }

    for (; it != end; it.increment(ec)) {
        if (cancel && cancel->load()) { errorMessage = "Operation cancelled."; return false; }
        if (ec) {
            errorMessage = "Folder traversal failed: " + ec.message();
            return false;
        }
        const fs::directory_entry current = *it;
        std::error_code statusError;
        const auto status = current.symlink_status(statusError);
        if (statusError) continue;
        // Do not follow symbolic links outside the mod directory.
        if (fs::is_symlink(status)) {
            if (fs::is_directory(current.status(statusError))) it.disable_recursion_pending();
            continue;
        }

        const fs::path relative = current.path().lexically_relative(source);
        const fs::path output = destination / relative;
        if (fs::is_directory(status)) {
            fs::create_directories(output, ec);
            if (ec) {
                errorMessage = "Could not create directory " + PathToUtf8(output) + ": " + ec.message();
                return false;
            }
        } else if (fs::is_regular_file(status)) {
            if (progress) {
                ++copiedFiles;
                progress("COPY_PROGRESS:" + std::to_string(copiedFiles) + ":" +
                         std::to_string(std::max<size_t>(1, totalFiles)) + "|Copying: " + PathToUtf8(relative));
            }
            fs::create_directories(output.parent_path(), ec);
            if (ec) {
                errorMessage = "Could not create directory " + PathToUtf8(output.parent_path()) + ": " + ec.message();
                return false;
            }
            fs::copy_file(current.path(), output, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                errorMessage = "Could not copy " + PathToUtf8(current.path()) + ": " + ec.message();
                return false;
            }
            // Keep source timestamps so the file-time update policy remains meaningful.
            const auto sourceTime = fs::last_write_time(current.path(), ec);
            if (!ec) fs::last_write_time(output, sourceTime, ec);
            ec.clear();
        }
    }
    if (ec) {
        errorMessage = "Folder traversal failed: " + ec.message();
        return false;
    }
    return true;
}

std::string LoadStatus(const ModEntry& entry) {
    if (entry.id.empty()) return "No mod.info ID";
    if (entry.loadOrder > 0) return "Enabled (#" + std::to_string(entry.loadOrder) + ")";
    return "Not in default.txt";
}

} // namespace pzmm
