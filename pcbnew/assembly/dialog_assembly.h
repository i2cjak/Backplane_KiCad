#ifndef PCBNEW_DIALOG_ASSEMBLY_H
#define PCBNEW_DIALOG_ASSEMBLY_H

#include "assembly_model.h"
#include <wx/dialog.h>
#include <wx/bookctrl.h>
#include <wx/grid.h>
#include <vector>
#include <functional>
#include <memory>

class wxGrid;
class wxNotebook;
class wxTextCtrl;
class BOARD;
class PCB_EDIT_FRAME;

class DIALOG_ASSEMBLY : public wxDialog
{
public:
    DIALOG_ASSEMBLY( wxWindow* aParent, ASSEMBLY_MANIFEST aManifest );
    ~DIALOG_ASSEMBLY() override;
    const ASSEMBLY_MANIFEST& Manifest() const { return m_manifest; }
    bool SaveManifest( const wxString& aPath );
    bool LoadManifest( const wxString& aPath );
    bool IsDirty() const { return m_dirty; }
    bool OpenTab( const wxString& aPath );
    nlohmann::json GetOpenTabs();
    bool ShowTab3D( size_t aTab );
    static DIALOG_ASSEMBLY* FindOrCreate( PCB_EDIT_FRAME* aParent );
    void SetClosedCallback( std::function<void()> aCallback ) { m_closedCallback = std::move( aCallback ); }

private:
    void OnAdd( wxCommandEvent& aEvent );
    void OnEdit( wxCommandEvent& aEvent );
    void OnGridChanged( wxGridEvent& aEvent );
    void OnRemove( wxCommandEvent& aEvent );
    void OnSave( wxCommandEvent& aEvent );
    void OnOpen( wxCommandEvent& aEvent );
    void OnExport( wxCommandEvent& aEvent );
    void OnCheck( wxCommandEvent& aEvent );
    void OnAddMate( wxCommandEvent& aEvent );
    void OnRemoveMate( wxCommandEvent& aEvent );
    void OnTabChanged( wxBookCtrlEvent& aEvent );
    void OnCancel( wxCommandEvent& aEvent );
    void OnCloseWindow( wxCloseEvent& aEvent );
    void OnView3D( wxCommandEvent& aEvent );
    void OnElectricalSync( wxCommandEvent& aEvent );
    void OnAddEndpoint( wxCommandEvent& aEvent );
    void OnRemoveEndpoint( wxCommandEvent& aEvent );
    void RefreshInstances();
    bool SyncFromGrid();
    bool StoreCurrentTab();
    bool StoreTab( size_t aIndex );
    void LoadTab( size_t aIndex );
    void SyncElectricalGrid();

    struct TAB_STATE
    {
        ASSEMBLY_MANIFEST manifest;
        wxString path;
        bool dirty = false;
        std::filesystem::path stagingDirectory;
        std::filesystem::path stagingStep;
    };
    ASSEMBLY_MANIFEST m_manifest;
    wxGrid* m_instances = nullptr;
    wxGrid* m_mates = nullptr;
    wxGrid* m_components = nullptr;
    wxGrid* m_electrical = nullptr;
    wxNotebook* m_tabs = nullptr;
    wxTextCtrl* m_status = nullptr;
    wxTextCtrl* m_name = nullptr;
    wxTextCtrl* m_anchor = nullptr;
    wxString m_manifestPath;
    std::vector<TAB_STATE> m_tabStates;
    bool m_dirty = false;
    std::function<void()> m_closedCallback;
    wxDECLARE_EVENT_TABLE();
};

#endif
