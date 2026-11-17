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
#include <wildcards_and_files_ext.h>
#include <wx/filename.h>
#include <nlohmann/json.hpp>

#include <wx/aui/framemanager.h>

#include <cmath>
#include <map>
#include <stdexcept>

namespace
{
using json = nlohmann::json;

// where a box is on screen now, from the docked window's content top-left (points): 1 in view (x, y, w, h, clipped
// to the view; a wire or a track a strip a robot can draw along), 2 out of view (x, y: the point at the view's edge
// toward it), 0 no view
int where_box( SCH_EDIT_FRAME* aFrame, const BOX2I& aBox, double& x, double& y, double& w, double& h )
{
    EDA_DRAW_PANEL_GAL* canvas = aFrame->GetCanvas();

    if( !canvas || !canvas->GetView() )
        return 0;

    KIGFX::VIEW*   view = canvas->GetView();
    const VECTOR2D a = view->ToScreen( VECTOR2D( aBox.GetLeft(), aBox.GetTop() ) );
    const VECTOR2D b = view->ToScreen( VECTOR2D( aBox.GetRight(), aBox.GetBottom() ) );
    const wxSize   client = canvas->GetClientSize();
    const VECTOR2I px = view->GetScreenPixelSize();
    const double   scale = client.x > 0 ? double( px.x ) / client.x : 1.0;
    const wxPoint  origin = canvas->ClientToScreen( wxPoint( 0, 0 ) ) - aFrame->ClientToScreen( wxPoint( 0, 0 ) );

    double x0 = std::min( a.x, b.x ) / scale, x1 = std::max( a.x, b.x ) / scale;
    double y0 = std::min( a.y, b.y ) / scale, y1 = std::max( a.y, b.y ) / scale;

    if( x1 < 0 || y1 < 0 || x0 > client.x || y0 > client.y )   // out of view: the edge, toward it
    {
        const double cx = client.x / 2.0, cy = client.y / 2.0, pad = 28;
        double       dx = ( x0 + x1 ) / 2 - cx, dy = ( y0 + y1 ) / 2 - cy;
        const double n = std::hypot( dx, dy );
        dx = n > 0 ? dx / n : 1;
        dy = n > 0 ? dy / n : 0;
        const double k = std::min( dx != 0 ? ( cx - pad ) / std::abs( dx ) : 1e9, dy != 0 ? ( cy - pad ) / std::abs( dy ) : 1e9 );
        x = origin.x + cx + dx * k;
        y = origin.y + cy + dy * k;
        w = h = 0;
        return 2;
    }

    x0 = std::max( 0.0, x0 );
    y0 = std::max( 0.0, y0 );
    x1 = std::min( double( client.x ), x1 );
    y1 = std::min( double( client.y ), y1 );

    if( x1 - x0 < 6 )
    {
        const double c = ( x0 + x1 ) / 2;
        x0 = c - 3;
        x1 = c + 3;
    }

    if( y1 - y0 < 6 )
    {
        const double c = ( y0 + y1 ) / 2;
        y0 = c - 3;
        y1 = c + 3;
    }

    x = origin.x + x0;
    y = origin.y + y0;
    w = x1 - x0;
    h = y1 - y0;
    return 1;
}

// the view Workbench last saw or set, per editor: once the user zooms or pans it themselves (for this file), the
// robots leave it alone (work out of view gets a robot at the view's edge instead of a fit)
struct VIEW_MARK
{
    void*    frame = nullptr;
    wxString file;
    VECTOR2D center;
    double   scale = 0;
    bool     user = false;
};

VIEW_MARK g_mark;

void mark_view( SCH_EDIT_FRAME* aFrame )
{
    KIGFX::VIEW* view = aFrame->GetCanvas() ? aFrame->GetCanvas()->GetView() : nullptr;

    if( !view )
        return;

    const wxString file = aFrame->Schematic().GetFileName();
    const bool     same = g_mark.frame == aFrame && g_mark.file == file;
    g_mark = { aFrame, file, view->GetCenter(), view->GetScale(), same && g_mark.user };
}

bool user_moved( SCH_EDIT_FRAME* aFrame )
{
    KIGFX::VIEW* view = aFrame->GetCanvas() ? aFrame->GetCanvas()->GetView() : nullptr;

    if( !view )
        return false;

    if( g_mark.frame != aFrame || g_mark.file != aFrame->Schematic().GetFileName() )
    {
        mark_view( aFrame );   // another file: its view as it opened
        return false;
    }

    if( !g_mark.user && g_mark.scale > 0 )
    {
        const double span = view->GetViewport().GetWidth();
        g_mark.user = std::abs( view->GetScale() - g_mark.scale ) > 1e-3 * g_mark.scale
                      || ( view->GetCenter() - g_mark.center ).EuclideanNorm() > 1e-3 * span;
    }

    return g_mark.user;
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

    KIGFX::VIEW* view = aFrame->GetCanvas()->GetView();
    const KIID   id = aItem->m_Uuid;
    // asked every frame while its robot works: the robot follows the item when the view is zoomed, panned or resized
    KIPLATFORM::WORKBENCH::SWARM_WHERE where = [aFrame, id]( double& x, double& y, double& w, double& h ) -> int
    {
        SCH_ITEM* item = aFrame->Schematic().ResolveItem( id, nullptr, true );
        return item ? where_box( aFrame, item->GetBoundingBox(), x, y, w, h ) : 0;
    };
    double x, y, w, h;

    if( !where( x, y, w, h ) )
        return;

    // new or changed, it's shown (where it now is) once its robot has drawn it out
    view->Hide( aItem, true );
    (void) aNew;

    KIPLATFORM::WORKBENCH::SwarmTargetLive( where,
            [aFrame, id]()
            {
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

    void SetFrame( SCH_EDIT_FRAME* aFrame ) { m_frame = aFrame; }   // a frame made again (after another project opened)

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
    if( !KIPLATFORM::WORKBENCH::Docked() )
        return;

    if( g_listener )
        g_listener->SetFrame( aFrame );

    if( g_watched == &aFrame->Schematic() )
        return;

    if( !g_listener )
        g_listener = new WORKBENCH_SCH_LISTENER( aFrame );

    g_listener->SetFrame( aFrame );

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
            if( user_moved( aFrame ) )   // the user has set the view themselves: it stays theirs
            {
                result = { { "fit", false }, { "note", "kept the user's own zoom and pan" } };
            }
            else
            {
                aFrame->GetToolManager()->RunAction( ACTIONS::zoomFitScreen );
                mark_view( aFrame );
                result = true;
            }
        }
        else if( cmd == "save" )
        {
            if( !aFrame->SaveProject() )
                throw std::runtime_error( "couldn't save the schematic" );

            result = { { "saved", true } };
        }
        else if( cmd == "reload" )
        {
            // the user's own zoom and pan (once they've moved the view) are kept through the reload
            const bool     userView = user_moved( aFrame );
            const VECTOR2D keepCenter = aFrame->GetCanvas()->GetView()->GetCenter();
            const double   keepScale = aFrame->GetCanvas()->GetView()->GetScale();
            const std::map<KIID, BOX2I> before = snapshot( aFrame );
            // the root sheet's file (the project's schematic when the sheet hasn't named one yet)
            wxString path = aFrame->Schematic().GetFileName();

            if( path.IsEmpty() || wxFileName( path ).GetExt() != wxString( FILEEXT::KiCadSchematicFileExtension ) )
            {
                wxFileName fn( aFrame->Prj().GetProjectFullName() );
                fn.SetExt( FILEEXT::KiCadSchematicFileExtension );
                path = fn.GetFullPath();
            }

            path = aFrame->Prj().AbsolutePath( path );

            // the files on disk win: nothing to ask about
            SCH_SCREENS screens( aFrame->Schematic().Root() );

            for( SCH_SCREEN* sc = screens.GetFirst(); sc; sc = screens.GetNext() )
                sc->SetContentModified( false );

            if( !aFrame->OpenProjectFiles( std::vector<wxString>{ path }, KICTL_REVERT ) )
                throw std::runtime_error( "couldn't reload the schematic" );

            if( userView )
            {
                aFrame->GetCanvas()->GetView()->SetScale( keepScale );
                aFrame->GetCanvas()->GetView()->SetCenter( keepCenter );
            }

            WorkbenchSchWatch( aFrame );
            int                                   added = 0, changed = 0;
            std::vector<std::pair<KIID, bool>>    work;   // what changed, and whether it's new
            BOX2I                                 where;

            for( const auto& [id, box] : snapshot( aFrame ) )
            {
                auto it = before.find( id );

                if( it == before.end() || it->second != box )
                {
                    work.emplace_back( id, it == before.end() );
                    where.Merge( box );
                    ++( it == before.end() ? added : changed );
                }
            }

            // the robots act it out where it can be seen: the whole sheet in view when some of it is outside
            if( !work.empty() && KIPLATFORM::WORKBENCH::SwarmOn() && !userView )
            {
                const BOX2D view = aFrame->GetCanvas()->GetView()->GetViewport();

                if( !view.Contains( BOX2D( VECTOR2D( where.GetOrigin() ), VECTOR2D( where.GetSize() ) ) ) )
                    aFrame->GetToolManager()->RunAction( ACTIONS::zoomFitScreen );
            }

            for( const auto& [id, isNew] : work )
                act_out( aFrame, aFrame->Schematic().ResolveItem( id, nullptr, true ), isNew );

            if( !userView )
                mark_view( aFrame );   // the view as Workbench left it

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
