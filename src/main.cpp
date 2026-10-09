#include "MainFrame.h"

class PZModManagerApp final : public wxApp {
public:
    bool OnInit() override {
        if (!wxApp::OnInit()) return false;
        wxInitAllImageHandlers();
        auto* frame = new MainFrame();
        frame->Centre();
        frame->Show(true);
        return true;
    }
};

wxIMPLEMENT_APP(PZModManagerApp);
