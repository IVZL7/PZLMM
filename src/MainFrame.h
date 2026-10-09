#pragma once

#include "ModScanner.h"

#include <atomic>
#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <thread>

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/statbmp.h>
#include <wx/thread.h>
#include <wx/gauge.h>
#include <wx/timer.h>

wxDECLARE_EVENT(wxEVT_PZMM_JOB_DONE, wxThreadEvent);
wxDECLARE_EVENT(wxEVT_PZMM_PROGRESS, wxThreadEvent);

class MainFrame final : public wxFrame {
public:
    MainFrame();
    ~MainFrame() override;

private:
    enum class PageKind { UserMods, WorkshopMods };

    struct PageWidgets {
        wxPanel* panel = nullptr;
        wxListCtrl* list = nullptr;
        wxStaticBitmap* poster = nullptr;
        wxTextCtrl* details = nullptr;
        std::vector<size_t> visibleIndices;
        int sortColumn = 0;
        bool sortAscending = true;
        std::map<int, wxString> filters;
        std::vector<wxString> columnTitles;
    };

    struct JobResult {
        pzmm::ScanResults scan;
        bool hasScan = false;
        bool showMessage = false;
        bool showArchiveNotification = false;
        bool success = true;
        std::string message;
        pzmm::fs::path archivePath;
    };

    struct CopyJob {
        pzmm::ModEntry source;
        std::optional<pzmm::ModEntry> installed;
        pzmm::fs::path targetPath;
    };

    PageWidgets BuildListPage(PageKind kind);
    void StartScan();
    void StartWorker(std::function<JobResult(const std::atomic<bool>&)> task);
    void OnJobDone(wxThreadEvent& event);
    void OnProgress(wxThreadEvent& event);
    void ReportProgress(const std::string& message);
    void OnProgressPulse(wxTimerEvent& event);
    void OnColumnClick(PageKind kind, wxListEvent& event);
    void OnColumnRightClick(PageKind kind, wxListEvent& event);
    void OnListRightClick(PageKind kind, wxListEvent& event);
    void OnBrowseUserPath(wxCommandEvent& event);
    void OnBrowseWorkshopPath(wxCommandEvent& event);
    void OnCopySelected(wxCommandEvent& event);
    void OnUpdateSelected(wxCommandEvent& event);
    void OnListSelection(PageKind kind, wxListEvent& event);
    void OnRefresh(wxCommandEvent& event);

    void PopulateAllLists();
    void PopulateList(PageKind kind, PageWidgets& page, const std::vector<pzmm::ModEntry>& mods);
    void RefreshListView(PageKind kind, PageWidgets& page, const std::vector<pzmm::ModEntry>& mods);
    wxString ColumnValue(PageKind kind, const pzmm::ModEntry& mod, int column) const;
    void UpdateDetails(PageKind kind, long row);
    wxString DescribeMod(const pzmm::ModEntry& entry, PageKind kind) const;
    void SetPoster(PageWidgets& page, const pzmm::fs::path& path);
    void SetBusy(bool busy);
    void StartCopyOrUpdate(bool updateOnly);
    void ArchiveSelectedMods(PageKind kind);
    void ShowArchiveNotification(const pzmm::fs::path& archivePath);
    void DeleteSelectedUserMods();
    std::vector<size_t> SelectedIndices(const PageWidgets& page) const;
    bool PathsMatchLastScan() const;

    wxTextCtrl* m_userPathCtrl = nullptr;
    wxTextCtrl* m_workshopPathCtrl = nullptr;
    wxButton* m_refreshButton = nullptr;
    wxButton* m_browseUserButton = nullptr;
    wxButton* m_browseWorkshopButton = nullptr;
    wxNotebook* m_notebook = nullptr;
    wxStatusBar* m_statusBar = nullptr;
    wxStaticText* m_activityText = nullptr;
    wxGauge* m_progressGauge = nullptr;
    wxTimer m_progressTimer;

    PageWidgets m_userPage;
    PageWidgets m_workshopPage;

    wxChoice* m_updateMethodChoice = nullptr;
    wxButton* m_copyButton = nullptr;
    wxButton* m_updateButton = nullptr;

    pzmm::ScanResults m_scanResults;
    bool m_hasScan = false;
    bool m_busy = false;
    std::thread m_worker;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_shuttingDown{false};
    std::atomic<long long> m_lastProgressReportMs{0};
};
