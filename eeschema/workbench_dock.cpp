/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// The schematic editor docked in Workbench (kiplatform/workbench.h): Workbench's commands to it (MAIL_WORKBENCH)
// and its build swarm, acted out on the schematic's real changes on the sheet shown: a new item stays hidden until
// a robot has drawn it out; a changed one gets a robot working over it. Changes come from the schematic's listeners
// and from reloading the file from disk after Claude edits it (compared item by item).

#include <kiplatform/workbench.h>
#include <kiway_player.h>
#include <sch_edit_frame.h>
#include <sch_item.h>
#include <sch_screen.h>
#include <schematic.h>
#include <tool/actions.h>
#include <tool/tool_manager.h>
#include <view/view.h>
#include <nlohmann/json.hpp>

#include <wx/aui/framemanager.h>

#include <map>
#include <stdexcept>

namespace
{
using json = nlohmann::json;

bool screen_rect( SCH_EDIT_FRAME* aFrame, const BOX2I& aBox, double& x, double& y, double& w, double& h )
{
    EDA_DRAW_PANEL_GAL* canvas = aFrame->GetCanvas();

    if( !canvas || !canvas->GetView() )
        return false;

    KIGFX::VIEW*   view = canvas->GetView();
    const VECTOR2D a = view->ToScreen( VECTOR2D( aBox.GetLeft(), aBox.GetTop() ) );
    const VECTOR2D b = view->ToScreen( VECTOR2D( aBox.GetRight(), aBox.GetBottom() ) );
    const wxSize   client = canvas->GetClientSize();
    const VECTOR2I px = view->GetScreenPixelSize();
    const double   scale = client.x > 0 ? double( px.x ) / client.x : 1.0;
    const wxPoint  origin = canvas->ClientToScreen( wxPoint( 0, 0 ) ) - aFrame->ClientToScreen( wxPoint( 0, 0 ) );

    double x0 = std::min( a.x, b.x ) / scale, x1 = std::max( a.x, b.x ) / scale;
    double y0 = std::min( a.y, b.y ) / scale, y1 = std::max( a.y, b.y ) / scale;
    x0 = std::max( 0.0, x0 );
    y0 = std::max( 0.0, y0 );
    x1 = std::min( double( client.x ), x1 );
    y1 = std::min( double( client.y ), y1 );

    if( x1 - x0 < 3 || y1 - y0 < 3 )
        return false;

    x = origin.x + x0;
    y = origin.y + y0;
    w = x1 - x0;
    h = y1 - y0;
    return true;
}

bool on_sheet_shown( SCH_EDIT_FRAME* aFrame, SCH_ITEM* aItem )
{
    SCH_SCREEN* screen = aFrame->GetScreen();
    return screen && aItem && aItem->GetParent() && screen->Items().contains( aItem );
}

void act_out( SCH_EDIT_FRAME* aFrame, SCH_ITEM* aItem, bool aNew )
{
    if( !KIPLATFORM::WORKBENCH::SwarmOn() || !on_sheet_shown( aFrame, aItem ) )
        return;

    double x, y, w, h;

    if( !screen_rect( aFrame, aItem->GetBoundingBox(), x, y, w, h ) )
        return;

    KIGFX::VIEW* view = aFrame->GetCanvas()->GetView();
    const KIID   id = aItem->m_Uuid;

    if( aNew )
        view->Hide( aItem, true );   // shown once its robot has drawn it out

    KIPLATFORM::WORKBENCH::SwarmTarget( x, y, w, h,
            [aFrame, id, aNew]()
            {
                if( !aNew )
                    return;

                if( SCH_ITEM* item = aFrame->Schematic().ResolveItem( id, nullptr, true ) )
                {
                    aFrame->GetCanvas()->GetView()->Hide( item, false );
                    aFrame->GetCanvas()->Refresh();
                }
            } );
}

class WORKBENCH_SCH_LISTENER : public SCHEMATIC_LISTENER
{
public:
    explicit WORKBENCH_SCH_LISTENER( SCH_EDIT_FRAME* aFrame ) : m_frame( aFrame ) {}

    void OnSchItemsAdded( SCHEMATIC&, std::vector<SCH_ITEM*>& aItems ) override
    {
        for( SCH_ITEM* item : aItems )
            act_out( m_frame, item, true );
    }
    void OnSchItemsChanged( SCHEMATIC&, std::vector<SCH_ITEM*>& aItems ) override
    {
        for( SCH_ITEM* item : aItems )
            act_out( m_frame, item, false );
    }

private:
    SCH_EDIT_FRAME* m_frame;
};

WORKBENCH_SCH_LISTENER*  g_listener = nullptr;
SCHEMATIC*               g_watched = nullptr;
std::map<wxString, bool> g_panes;

std::map<KIID, BOX2I> snapshot( SCH_EDIT_FRAME* aFrame )
{
    std::map<KIID, BOX2I> out;

    if( SCH_SCREEN* screen = aFrame->GetScreen() )
    {
        for( SCH_ITEM* item : screen->Items() )
            out[item->m_Uuid] = item->GetBoundingBox();
    }

    return out;
}

// a pane of the manager's list (its elements are panes or pointers to them, by wxWidgets build)
inline wxAuiPaneInfo& pane_ref( wxAuiPaneInfo& aPane ) { return aPane; }
inline wxAuiPaneInfo& pane_ref( wxAuiPaneInfo* aPane ) { return *aPane; }
wxAuiPaneInfo& pane_at( wxAuiPaneInfoArray& aPanes, size_t aIndex ) { return pane_ref( aPanes[aIndex] ); }

void set_hands( SCH_EDIT_FRAME* aFrame, bool aOff )
{
    wxAuiManager* mgr = wxAuiManager::GetManager( aFrame );

    if( !mgr )
        return;

    wxAuiPaneInfoArray& panes = mgr->GetAllPanes();

    for( size_t i = 0; i < panes.GetCount(); ++i )
    {
        wxAuiPaneInfo& pane = pane_at( panes, i );

        if( pane.name == wxT( "DrawFrame" ) )
            continue;

        if( aOff )
        {
            if( !g_panes.count( pane.name ) )
                g_panes[pane.name] = pane.IsShown();

            pane.Show( false );
        }
        else if( g_panes.count( pane.name ) )
        {
            pane.Show( g_panes[pane.name] );
        }
    }

    if( !aOff )
        g_panes.clear();

    mgr->Update();
}
} // namespace


void WorkbenchSchWatch( SCH_EDIT_FRAME* aFrame )
{
    if( !KIPLATFORM::WORKBENCH::Docked() || g_watched == &aFrame->Schematic() )
        return;

    if( !g_listener )
        g_listener = new WORKBENCH_SCH_LISTENER( aFrame );

    aFrame->Schematic().AddListener( g_listener );
    g_watched = &aFrame->Schematic();
}


void WorkbenchSchMail( SCH_EDIT_FRAME* aFrame, std::string& aPayload )
{
    json reply;

    try
    {
        const json        req = json::parse( aPayload );
        const std::string cmd = req.value( "cmd", "" );
        json              result;

        if( cmd == "hands" )
        {
            set_hands( aFrame, req.value( "off", true ) );
            result = { { "hands_off", req.value( "off", true ) } };
        }
        else if( cmd == "fit" )
        {
            aFrame->GetToolManager()->RunAction( ACTIONS::zoomFitScreen );
            result = true;
        }
        else if( cmd == "save" )
        {
            if( !aFrame->SaveProject() )
                throw std::runtime_error( "couldn't save the schematic" );

            result = { { "saved", true } };
        }
        else if( cmd == "reload" )
        {
            const std::map<KIID, BOX2I> before = snapshot( aFrame );
            const wxString              file = aFrame->Schematic().Root().GetFileName();
            wxString                    path = aFrame->Prj().AbsolutePath( file );

            // the files on disk win: nothing to ask about
            SCH_SCREENS screens( aFrame->Schematic().Root() );

            for( SCH_SCREEN* sc = screens.GetFirst(); sc; sc = screens.GetNext() )
                sc->SetContentModified( false );

            if( !aFrame->OpenProjectFiles( std::vector<wxString>{ path }, KICTL_REVERT ) )
                throw std::runtime_error( "couldn't reload the schematic" );

            WorkbenchSchWatch( aFrame );
            int added = 0, changed = 0;

            for( const auto& [id, box] : snapshot( aFrame ) )
            {
                auto it = before.find( id );
                SCH_ITEM* item = aFrame->Schematic().ResolveItem( id, nullptr, true );

                if( it == before.end() )
                {
                    act_out( aFrame, item, true );
                    ++added;
                }
                else if( it->second != box )
                {
                    act_out( aFrame, item, false );
                    ++changed;
                }
            }

            aFrame->GetCanvas()->Refresh();
            result = { { "added", added }, { "changed", changed } };
        }
        else if( cmd == "inspect" )
        {
            SCH_SCREEN* screen = aFrame->GetScreen();
            result = { { "file", std::string( screen ? screen->GetFileName().ToUTF8() : "" ) },
                       { "items", screen ? screen->Items().size() : 0 },
                       { "modified", aFrame->IsContentModified() } };
        }
        else
        {
            throw std::runtime_error( "unknown command: " + cmd );
        }

        reply = { { "ok", true }, { "result", result } };
    }
    catch( const std::exception& e )
    {
        reply = { { "ok", false }, { "error", e.what() } };
    }

    aPayload = reply.dump();
}
