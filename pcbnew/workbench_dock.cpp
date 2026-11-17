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

// The PCB editor docked in Workbench (kiplatform/workbench.h): Workbench's commands to it (MAIL_WORKBENCH) and its
// build swarm, acted out on the board's real changes: a new item stays hidden until a robot has drawn it out; a
// changed one gets a robot working over it. Changes come from the board's listeners (edits in the editor, KiCad's
// IPC API) and from reloading the file from disk after Claude edits it (compared item by item).

#include <board.h>
#include <board_item.h>
#include <footprint.h>
#include <kiplatform/workbench.h>
#include <pcb_edit_frame.h>
#include <pcb_track.h>
#include <tool/actions.h>
#include <tool/tool_manager.h>
#include <view/view.h>
#include <zone.h>
#include <kiway_player.h>
#include <nlohmann/json.hpp>

#include <wx/aui/framemanager.h>

#include <map>
#include <set>
#include <stdexcept>

namespace
{
using json = nlohmann::json;

// where a box on the board is on screen, from the docked window's content top-left (points)
bool screen_rect( PCB_EDIT_FRAME* aFrame, const BOX2I& aBox, double& x, double& y, double& w, double& h )
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

void act_out( PCB_EDIT_FRAME* aFrame, BOARD_ITEM* aItem, bool aNew )
{
    if( !KIPLATFORM::WORKBENCH::SwarmOn() || !aItem )
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

                BOARD* board = aFrame->GetBoard();

                if( BOARD_ITEM* item = board ? board->ResolveItem( id, true ) : nullptr )
                {
                    aFrame->GetCanvas()->GetView()->Hide( item, false );
                    aFrame->GetCanvas()->Refresh();
                }
            } );
}

class WORKBENCH_BOARD_LISTENER : public BOARD_LISTENER
{
public:
    explicit WORKBENCH_BOARD_LISTENER( PCB_EDIT_FRAME* aFrame ) : m_frame( aFrame ) {}

    void OnBoardItemAdded( BOARD&, BOARD_ITEM* aItem ) override { act_out( m_frame, aItem, true ); }
    void OnBoardItemsAdded( BOARD&, std::vector<BOARD_ITEM*>& aItems ) override
    {
        for( BOARD_ITEM* item : aItems )
            act_out( m_frame, item, true );
    }
    void OnBoardItemChanged( BOARD&, BOARD_ITEM* aItem ) override { act_out( m_frame, aItem, false ); }
    void OnBoardItemsChanged( BOARD&, std::vector<BOARD_ITEM*>& aItems ) override
    {
        for( BOARD_ITEM* item : aItems )
            act_out( m_frame, item, false );
    }

private:
    PCB_EDIT_FRAME* m_frame;
};

WORKBENCH_BOARD_LISTENER* g_listener = nullptr;
BOARD*                    g_watched = nullptr;
std::map<wxString, bool>  g_panes;   // pane visibility before Hands off

// every item worth a robot, with its box
std::map<KIID, BOX2I> snapshot( BOARD* aBoard )
{
    std::map<KIID, BOX2I> out;

    if( !aBoard )
        return out;

    for( FOOTPRINT* fp : aBoard->Footprints() )
        out[fp->m_Uuid] = fp->GetBoundingBox();

    for( PCB_TRACK* t : aBoard->Tracks() )
        out[t->m_Uuid] = t->GetBoundingBox();

    for( BOARD_ITEM* d : aBoard->Drawings() )
        out[d->m_Uuid] = d->GetBoundingBox();

    for( ZONE* z : aBoard->Zones() )
        out[z->m_Uuid] = z->GetBoundingBox();

    return out;
}

// a pane of the manager's list (its elements are panes or pointers to them, by wxWidgets build)
inline wxAuiPaneInfo& pane_ref( wxAuiPaneInfo& aPane ) { return aPane; }
inline wxAuiPaneInfo& pane_ref( wxAuiPaneInfo* aPane ) { return *aPane; }
wxAuiPaneInfo& pane_at( wxAuiPaneInfoArray& aPanes, size_t aIndex ) { return pane_ref( aPanes[aIndex] ); }

void set_hands( PCB_EDIT_FRAME* aFrame, bool aOff )
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


void WorkbenchPcbWatch( PCB_EDIT_FRAME* aFrame )
{
    if( !KIPLATFORM::WORKBENCH::Docked() || !aFrame->GetBoard() || g_watched == aFrame->GetBoard() )
        return;

    if( !g_listener )
        g_listener = new WORKBENCH_BOARD_LISTENER( aFrame );

    aFrame->GetBoard()->AddListener( g_listener );
    g_watched = aFrame->GetBoard();
}


void WorkbenchPcbMail( PCB_EDIT_FRAME* aFrame, std::string& aPayload )
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
            if( !aFrame->SaveBoard() )
                throw std::runtime_error( "couldn't save the board" );

            result = { { "saved", std::string( aFrame->GetBoard()->GetFileName().ToUTF8() ) } };
        }
        else if( cmd == "reload" )
        {
            // the board as Claude left it on disk; what changed gets a robot (new items drawn out by theirs)
            const std::map<KIID, BOX2I> before = snapshot( aFrame->GetBoard() );
            const wxString              file = aFrame->GetBoard()->GetFileName();

            aFrame->GetScreen()->SetContentModified( false );   // the file on disk wins: nothing to ask about
            if( !aFrame->OpenProjectFiles( std::vector<wxString>{ file }, KICTL_REVERT ) )
                throw std::runtime_error( "couldn't reload the board" );

            WorkbenchPcbWatch( aFrame );
            BOARD* board = aFrame->GetBoard();
            int    added = 0, changed = 0;

            for( const auto& [id, box] : snapshot( board ) )
            {
                auto it = before.find( id );

                if( it == before.end() )
                {
                    act_out( aFrame, board->ResolveItem( id, true ), true );
                    ++added;
                }
                else if( it->second != box )
                {
                    act_out( aFrame, board->ResolveItem( id, true ), false );
                    ++changed;
                }
            }

            aFrame->GetCanvas()->Refresh();
            result = { { "added", added }, { "changed", changed } };
        }
        else if( cmd == "inspect" )
        {
            BOARD* board = aFrame->GetBoard();
            result = { { "file", std::string( board->GetFileName().ToUTF8() ) },
                       { "footprints", board->Footprints().size() },
                       { "tracks", board->Tracks().size() },
                       { "drawings", board->Drawings().size() },
                       { "zones", board->Zones().size() },
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
