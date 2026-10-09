#include "MainFrame.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <exception>
#include <set>
#include <sstream>
#include <utility>
#include <map>

#include <wx/dirdlg.h>
#include <wx/filefn.h>
#include <wx/utils.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>

wxDEFINE_EVENT(wxEVT_PZMM_JOB_DONE, wxThreadEvent);
wxDEFINE_EVENT(wxEVT_PZMM_PROGRESS, wxThreadEvent);

namespace {

wxString FromUtf8(const std::string& value) {
    return wxString::FromUTF8(value.c_str());
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

    bool archiveCreated = false;
    {
        wxFileOutputStream output(PathToWx(archivePath));
        if (!output.IsOk()) {
            error = "Could not create " + pzmm::PathToUtf8(archivePath) + ".";
        } else {
            wxZipOutputStream zip(output);
            std::set<std::string> archiveRoots;
            size_t fileCount = 0;
            for (const auto& mod : mods) {
                if (!error.empty()) break;
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
                    const std::string entryName = root + "/" + relativeName;
                    if (progress) progress("Archiving: " + entryName);
                    if (!zip.PutNextEntry(FromUtf8(entryName))) {
                        error = "Could not add " + entryName + " to the archive.";
                        break;
                    }
                    wxFileInputStream input(PathToWx(it->path()));
                    if (!input.IsOk()) {
                        error = "Could not read " + pzmm::PathToUtf8(it->path()) + ".";
                        break;
                    }
                    zip.Write(input);
                    if (!zip.IsOk()) {
                        error = "Writing the archive failed while adding " + entryName + ".";
                        break;
                    }
                    ++fileCount;
                }
                if (cancel.load()) error = "Archive creation was cancelled.";
                if (walkError && error.empty())
                    error = "Could not scan " + pzmm::PathToUtf8(mod.folderPath) + ": " + walkError.message();
            }

            if (error.empty() && fileCount == 0) error = "No files were found in the selected mod folder(s).";
            const bool zipClosed = zip.Close();
            if (error.empty() && (!zipClosed || !output.IsOk())) error = "Finalizing the ZIP archive failed.";
            archiveCreated = error.empty();
        }
    }
    if (!archiveCreated) {
        std::error_code removeError;
        pzmm::fs::remove(archivePath, removeError);
        return false;
    }
    return true;
}

} // namespace

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "Project Zomboid Mod Manager", wxDefaultPosition, wxSize(1280, 820)),
      m_progressTimer(this) {
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
    m_refreshButton = new wxButton(root, wxID_ANY, "Refresh scan");
    headerSizer->Add(m_refreshButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    rootSizer->Add(headerSizer, 0, wxEXPAND | wxALL, 8);

    auto* pathSizer = new wxFlexGridSizer(2, 3, 6, 8);
    pathSizer->AddGrowableCol(1, 1);
    pathSizer->Add(new wxStaticText(root, wxID_ANY, "User mods folder:"), 0, wxALIGN_CENTER_VERTICAL);
    m_userPathCtrl = new wxTextCtrl(root, wxID_ANY, DefaultUserModsPath());
    pathSizer->Add(m_userPathCtrl, 1, wxEXPAND);
    m_browseUserButton = new wxButton(root, wxID_ANY, "Browse...");
    pathSizer->Add(m_browseUserButton, 0);

    pathSizer->Add(new wxStaticText(root, wxID_ANY, "Steam Workshop folder:"), 0, wxALIGN_CENTER_VERTICAL);
    m_workshopPathCtrl = new wxTextCtrl(root, wxID_ANY, DefaultWorkshopPath());
    pathSizer->Add(m_workshopPathCtrl, 1, wxEXPAND);
    m_browseWorkshopButton = new wxButton(root, wxID_ANY, "Browse...");
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
    m_refreshButton->Bind(wxEVT_BUTTON, &MainFrame::OnRefresh, this);
    Bind(wxEVT_PZMM_JOB_DONE, &MainFrame::OnJobDone, this);
    Bind(wxEVT_PZMM_PROGRESS, &MainFrame::OnProgress, this);
    Bind(wxEVT_TIMER, &MainFrame::OnProgressPulse, this, m_progressTimer.GetId());

    StartScan();
}

MainFrame::~MainFrame() {
    m_shuttingDown.store(true);
    m_progressTimer.Stop();
    m_cancel.store(true);
    if (m_worker.joinable()) m_worker.join();
}

MainFrame::PageWidgets MainFrame::BuildListPage(PageKind kind) {
    PageWidgets widgets;
    widgets.panel = new wxPanel(m_notebook);
    auto* outerSizer = new wxBoxSizer(wxVERTICAL);
    auto* contentSizer = new wxBoxSizer(wxHORIZONTAL);

    widgets.list = new wxListCtrl(widgets.panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                  wxLC_REPORT | wxLC_HRULES | wxLC_VRULES);
    widgets.list->InsertColumn(0, "Mod name", wxLIST_FORMAT_LEFT, 190);
    widgets.list->InsertColumn(1, "Mod ID", wxLIST_FORMAT_LEFT, 165);
    widgets.list->InsertColumn(2, "Game versions", wxLIST_FORMAT_LEFT, 110);
    widgets.list->InsertColumn(3, "default.txt", wxLIST_FORMAT_LEFT, 120);
    if (kind == PageKind::WorkshopMods) {
        widgets.list->InsertColumn(4, "Locally installed", wxLIST_FORMAT_LEFT, 120);
        widgets.list->InsertColumn(5, "Workshop ID", wxLIST_FORMAT_LEFT, 100);
        widgets.columnTitles = {"Mod name", "Mod ID", "Game versions", "default.txt",
                                "Locally installed", "Workshop ID"};
    } else {
        widgets.list->InsertColumn(4, "Workshop ID", wxLIST_FORMAT_LEFT, 100);
        widgets.columnTitles = {"Mod name", "Mod ID", "Game versions", "default.txt", "Workshop ID"};
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
        m_updateButton = new wxButton(widgets.panel, wxID_ANY, "Update if Newer");
        actionSizer->Add(m_copyButton, 0, wxRIGHT, 6);
        actionSizer->Add(m_updateButton, 0, wxRIGHT, 5);
        outerSizer->Add(actionSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        m_copyButton->Bind(wxEVT_BUTTON, &MainFrame::OnCopySelected, this);
        m_updateButton->Bind(wxEVT_BUTTON, &MainFrame::OnUpdateSelected, this);
    }

    widgets.panel->SetSizer(outerSizer);
    widgets.list->Bind(wxEVT_LIST_COL_CLICK, [this, kind](wxListEvent& event) {
        OnColumnClick(kind, event);
    });
    widgets.list->Bind(wxEVT_LIST_COL_RIGHT_CLICK, [this, kind](wxListEvent& event) {
        OnColumnRightClick(kind, event);
    });
    widgets.list->Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, [this, kind](wxListEvent& event) {
        OnListRightClick(kind, event);
    });
    widgets.list->Bind(wxEVT_LIST_ITEM_SELECTED, [this, kind](wxListEvent& event) {
        OnListSelection(kind, event);
    });
    widgets.list->Bind(wxEVT_LIST_ITEM_DESELECTED, [this, kind](wxListEvent& event) {
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
        result.scan = pzmm::ScanAll(userRoot, workshopRoot, cancel, [this](const std::string& text) { ReportProgress(text); });
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

void MainFrame::OnColumnClick(PageKind kind, wxListEvent& event) {
    PageWidgets* page = kind == PageKind::UserMods ? &m_userPage : &m_workshopPage;
    const int col = event.GetColumn();
    if (col < 0 || static_cast<size_t>(col) >= page->columnTitles.size()) return;
    if (page->sortColumn == col) page->sortAscending = !page->sortAscending;
    else { page->sortColumn = col; page->sortAscending = true; }
    const auto& mods = kind == PageKind::UserMods ? m_scanResults.userMods : m_scanResults.workshopMods;
    RefreshListView(kind, *page, mods);
}

void MainFrame::OnColumnRightClick(PageKind kind, wxListEvent& event) {
    PageWidgets* page = kind == PageKind::UserMods ? &m_userPage : &m_workshopPage;
    const int col = event.GetColumn();
    if (col < 0 || static_cast<size_t>(col) >= page->columnTitles.size()) return;
    wxMenu menu;
    menu.Append(7101, "Filter by substring...");
    menu.Append(7102, "Clear this column filter");
    menu.Append(7103, "Clear all filters");
    menu.Bind(wxEVT_MENU, [this, page, kind, col](wxCommandEvent& command) {
        if (command.GetId() == 7101) {
            wxTextEntryDialog dialog(this, "Show rows where this column contains:",
                                     "Filter: " + page->columnTitles[col], page->filters[col]);
            if (dialog.ShowModal() == wxID_OK) {
                const wxString value = dialog.GetValue().Lower();
                if (value.empty()) page->filters.erase(col);
                else page->filters[col] = value;
                const auto& mods = kind == PageKind::UserMods ? m_scanResults.userMods : m_scanResults.workshopMods;
                RefreshListView(kind, *page, mods);
            }
        } else if (command.GetId() == 7102) {
            page->filters.erase(col);
            const auto& mods = kind == PageKind::UserMods ? m_scanResults.userMods : m_scanResults.workshopMods;
            RefreshListView(kind, *page, mods);
        } else if (command.GetId() == 7103) {
            page->filters.clear();
            const auto& mods = kind == PageKind::UserMods ? m_scanResults.userMods : m_scanResults.workshopMods;
            RefreshListView(kind, *page, mods);
        }
    });
    PopupMenu(&menu);
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
    menu.Append(7200, "Show in Explorer");
    menu.Append(7204, "Archive selected mod(s) to ZIP...");
    menu.AppendSeparator();
    menu.Bind(wxEVT_MENU, [folderPath](wxCommandEvent&) { OpenInExplorer(folderPath); }, 7200);
    menu.Bind(wxEVT_MENU, [this, kind](wxCommandEvent&) { ArchiveSelectedMods(kind); }, 7204);
    if (kind == PageKind::UserMods) {
        menu.Append(7201, "Delete selected mod(s)...");
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { DeleteSelectedUserMods(); }, 7201);
    } else {
        menu.Append(7202, "Copy selected mod(s)...");
        menu.Append(7203, "Update selected if newer...");
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
            const auto target = pzmm::ResolveTarget(mod, m_scanResults.userMods, m_scanResults.userModsPath);
            return target.ambiguousId ? "Ambiguous ID" : (target.installed ? "Installed" : "Missing");
        }
        return FromUtf8(mod.workshopItemId);
    case 5: return FromUtf8(mod.workshopItemId);
    default: return {};
    }
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

void MainFrame::OnCopySelected(wxCommandEvent&) {
    StartCopyOrUpdate(false);
}

void MainFrame::OnUpdateSelected(wxCommandEvent&) {
    StartCopyOrUpdate(true);
}

void MainFrame::OnRefresh(wxCommandEvent&) {
    StartScan();
}

void MainFrame::OnListSelection(PageKind kind, wxListEvent& event) {
    UpdateDetails(kind, event.GetIndex());
}

void MainFrame::PopulateAllLists() {
    PopulateList(PageKind::UserMods, m_userPage, m_scanResults.userMods);
    PopulateList(PageKind::WorkshopMods, m_workshopPage, m_scanResults.workshopMods);
}

void MainFrame::PopulateList(PageKind kind, PageWidgets& page, const std::vector<pzmm::ModEntry>& mods) {
    page.visibleIndices.clear();
    page.list->DeleteAllItems();
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
    for (size_t col = 1; col < page.columnTitles.size(); ++col)
            page.list->SetItem(row, static_cast<int>(col), ColumnValue(kind, mod, static_cast<int>(col)));
        page.list->SetItemData(row, static_cast<wxUIntPtr>(sourceIndex));
    }
    // Requested arrow convention: down = ascending, up = descending.
    for (size_t col = 0; col < page.columnTitles.size(); ++col) {
        wxString title = page.columnTitles[col];
        if (static_cast<int>(col) == page.sortColumn)
            title += page.sortAscending ? " ↓" : " ↑";
        wxListItem item;
        item.SetMask(wxLIST_MASK_TEXT);
        item.SetText(title);
        page.list->SetColumn(static_cast<int>(col), item);
    }
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
    m_userPathCtrl->Enable(!busy);
    m_workshopPathCtrl->Enable(!busy);
    m_browseUserButton->Enable(!busy);
    m_browseWorkshopButton->Enable(!busy);
    m_notebook->Enable(true);
    if (m_copyButton) m_copyButton->Enable(!busy);
    if (m_updateButton) m_updateButton->Enable(!busy);
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
            if (pzmm::CopyDirectoryContents(job.source.folderPath, job.targetPath, copyError, &cancel,
                    [this](const std::string& text) { ReportProgress(text); })) {
                ++completed;
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
        result.scan = pzmm::ScanAll(userRoot, workshopRoot, cancel, [this](const std::string& text) { ReportProgress(text); });
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
