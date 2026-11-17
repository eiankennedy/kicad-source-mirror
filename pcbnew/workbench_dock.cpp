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
#include <pad.h>
#include <pcb_field.h>
#include <pcb_shape.h>
#include <pcb_text.h>
#include <geometry/shape_arc.h>
#include <geometry/shape_line_chain.h>
#include <kiway_player.h>
#include <nlohmann/json.hpp>

#include <wx/aui/framemanager.h>

#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace
{
using json = nlohmann::json;

// where a box is on screen now, from the docked window's content top-left (points): 1 in view (x, y, w, h, clipped
// to the view; a wire or a track a strip a robot can draw along), 2 out of view (x, y: the point at the view's edge
// toward it), 0 no view
int where_box( PCB_EDIT_FRAME* aFrame, const BOX2I& aBox, double& x, double& y, double& w, double& h )
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

void mark_view( PCB_EDIT_FRAME* aFrame )
{
    KIGFX::VIEW* view = aFrame->GetCanvas() ? aFrame->GetCanvas()->GetView() : nullptr;

    if( !view )
        return;

    const wxString file = aFrame->GetBoard()->GetFileName();
    const bool     same = g_mark.frame == aFrame && g_mark.file == file;
    g_mark = { aFrame, file, view->GetCenter(), view->GetScale(), same && g_mark.user };
}

bool user_moved( PCB_EDIT_FRAME* aFrame )
{
    KIGFX::VIEW* view = aFrame->GetCanvas() ? aFrame->GetCanvas()->GetView() : nullptr;

    if( !view )
        return false;

    if( g_mark.frame != aFrame || g_mark.file != aFrame->GetBoard()->GetFileName() )
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

// ---------------------------------------------------------------- the build plan

using PIECE = KIPLATFORM::WORKBENCH::SWARM_PIECE;
using PT = KIPLATFORM::WORKBENCH::SWARM_PT;

// what the swarm keeps out of sight until its robot has made it (a reload makes the board again: hidden again then)
std::set<KIID> g_pending;

// the board's world (nm) to the overlay (points, from the docked window's content top-left), this frame
KIPLATFORM::WORKBENCH::SWARM_VIEW view_map( PCB_EDIT_FRAME* aFrame )
{
    return [aFrame]( double* m ) -> bool
    {
        EDA_DRAW_PANEL_GAL* canvas = aFrame->GetCanvas();

        if( !canvas || !canvas->GetView() )
            return false;

        KIGFX::VIEW*   view = canvas->GetView();
        const wxSize   client = canvas->GetClientSize();
        const VECTOR2I px = view->GetScreenPixelSize();
        const double   scale = client.x > 0 ? double( px.x ) / client.x : 1.0;
        const wxPoint  origin = canvas->ClientToScreen( wxPoint( 0, 0 ) ) - aFrame->ClientToScreen( wxPoint( 0, 0 ) );
        const VECTOR2D o = view->ToScreen( VECTOR2D( 0, 0 ) );
        const VECTOR2D ex = view->ToScreen( VECTOR2D( 1e6, 0 ) ) - o;
        const VECTOR2D ey = view->ToScreen( VECTOR2D( 0, 1e6 ) ) - o;
        m[0] = ex.x / 1e6 / scale;
        m[1] = ey.x / 1e6 / scale;
        m[2] = origin.x + o.x / scale;
        m[3] = ex.y / 1e6 / scale;
        m[4] = ey.y / 1e6 / scale;
        m[5] = origin.y + o.y / scale;
        return true;
    };
}

void hide( KIGFX::VIEW* aView, BOARD_ITEM* aItem )
{
    aView->Hide( aItem, true );
    g_pending.insert( aItem->m_Uuid );
}

// an item out of sight with everything in it (a footprint's pads, texts and graphics are view items of their own)
void hide_all( KIGFX::VIEW* aView, BOARD_ITEM* aItem )
{
    hide( aView, aItem );
    aItem->RunOnChildren( [&]( BOARD_ITEM* aChild ) { hide( aView, aChild ); }, RECURSE_MODE::RECURSE );
}

void reveal( PCB_EDIT_FRAME* aFrame, const KIID& aId )
{
    g_pending.erase( aId );
    BOARD* board = aFrame->GetBoard();

    if( BOARD_ITEM* item = board ? board->ResolveItem( aId, true ) : nullptr )
    {
        aFrame->GetCanvas()->GetView()->Hide( item, false );
        aFrame->GetCanvas()->Refresh();
    }
}

void reveal_all( PCB_EDIT_FRAME* aFrame, const KIID& aId )
{
    BOARD* board = aFrame->GetBoard();
    BOARD_ITEM* item = board ? board->ResolveItem( aId, true ) : nullptr;

    if( !item )
    {
        g_pending.erase( aId );
        return;
    }

    std::vector<KIID> ids{ aId };
    item->RunOnChildren( [&]( BOARD_ITEM* aChild ) { ids.push_back( aChild->m_Uuid ); }, RECURSE_MODE::RECURSE );

    for( const KIID& id : ids )
        reveal( aFrame, id );
}

// after a reload (the board made again from the file): what the robots haven't made yet stays out of sight
void hide_pending( PCB_EDIT_FRAME* aFrame )
{
    BOARD* board = aFrame->GetBoard();

    for( auto it = g_pending.begin(); it != g_pending.end(); )
    {
        if( BOARD_ITEM* item = board ? board->ResolveItem( *it, true ) : nullptr )
        {
            aFrame->GetCanvas()->GetView()->Hide( item, true );
            ++it;
        }
        else
        {
            it = g_pending.erase( it );
        }
    }
}

PT pt( const VECTOR2I& p )
{
    return PT{ double( p.x ), double( p.y ) };
}

const PT LIFT{ NAN, NAN };

// a shape's line as points (one stroke)
void shape_points( const EDA_SHAPE& s, std::vector<PT>& out )
{
    switch( s.GetShape() )
    {
    case SHAPE_T::SEGMENT:
        out.push_back( pt( s.GetStart() ) );
        out.push_back( pt( s.GetEnd() ) );
        break;
    case SHAPE_T::RECTANGLE:
    {
        const std::vector<VECTOR2I> c = s.GetRectCorners();
        for( const VECTOR2I& p : c )
            out.push_back( pt( p ) );
        if( !c.empty() )
            out.push_back( pt( c.front() ) );
        break;
    }
    case SHAPE_T::CIRCLE:
    {
        const VECTOR2I c = s.GetStart();   // a circle: its centre
        const double   r = s.GetRadius();
        for( int i = 0; i <= 24; ++i )
            out.push_back( PT{ c.x + r * std::cos( i * M_PI / 12 ), c.y + r * std::sin( i * M_PI / 12 ) } );
        break;
    }
    case SHAPE_T::ARC:
    {
        const SHAPE_LINE_CHAIN ch = SHAPE_ARC( s.GetStart(), s.GetArcMid(), s.GetEnd(), 0 ).ConvertToPolyline();
        for( int i = 0; i < ch.PointCount(); ++i )
            out.push_back( pt( ch.CPoint( i ) ) );
        break;
    }
    case SHAPE_T::POLY:
    {
        const SHAPE_POLY_SET& ps = s.GetPolyShape();
        for( int o = 0; o < ps.OutlineCount(); ++o )
        {
            if( o > 0 )
                out.push_back( LIFT );
            const SHAPE_LINE_CHAIN& ch = ps.COutline( o );
            for( int i = 0; i < ch.PointCount(); ++i )
                out.push_back( pt( ch.CPoint( i ) ) );
            if( ch.PointCount() )
                out.push_back( pt( ch.CPoint( 0 ) ) );
        }
        break;
    }
    case SHAPE_T::BEZIER:
        for( const VECTOR2I& p : s.GetBezierPoints() )
            out.push_back( pt( p ) );
        break;
    default: break;
    }
}

void box_points( const BOX2I& b, std::vector<PT>& out )
{
    out.push_back( PT{ double( b.GetLeft() ), double( b.GetTop() ) } );
    out.push_back( PT{ double( b.GetRight() ), double( b.GetTop() ) } );
    out.push_back( PT{ double( b.GetRight() ), double( b.GetBottom() ) } );
    out.push_back( PT{ double( b.GetLeft() ), double( b.GetBottom() ) } );
    out.push_back( PT{ double( b.GetLeft() ), double( b.GetTop() ) } );
}

unsigned layer_color( PCB_LAYER_ID aLayer )
{
    switch( aLayer )
    {
    case F_Cu: return 0xC83434;
    case B_Cu: return 0x4D7FC4;
    case Edge_Cuts: return 0xD0D2CD;
    case F_SilkS:
    case B_SilkS: return 0xF2EDA1;
    case F_CrtYd:
    case B_CrtYd: return 0xFF26E2;
    default: return IsCopperLayer( aLayer ) ? 0x7FC87F : 0;
    }
}

int add( std::vector<PIECE>& aPieces, PIECE aPiece )
{
    aPieces.push_back( std::move( aPiece ) );
    return int( aPieces.size() ) - 1;
}

// shapes traced in a few pieces (several strokes each), each shape shown as its piece is done
std::vector<int> trace_shapes( PCB_EDIT_FRAME* aFrame, std::vector<PIECE>& aPieces, const std::vector<PCB_SHAPE*>& aShapes,
                               int aGroups, const std::vector<int>& aAfter, int aHold, const std::string& aTag )
{
    std::vector<int> out;
    const size_t     n = aShapes.size();
    const size_t     per = std::max<size_t>( 1, ( n + aGroups - 1 ) / std::max( 1, aGroups ) );

    for( size_t i = 0; i < n; i += per )
    {
        PIECE             p;
        std::vector<KIID> ids;
        p.kind = PIECE::TRACE;
        p.after = aAfter;
        p.hold = aHold;
        p.tag = out.empty() ? aTag : std::string();

        for( size_t k = i; k < std::min( n, i + per ); ++k )
        {
            if( !p.pts.empty() )
                p.pts.push_back( LIFT );

            shape_points( *aShapes[k], p.pts );
            ids.push_back( aShapes[k]->m_Uuid );
            p.width = std::max( p.width, double( aShapes[k]->GetWidth() ) );
            p.color = layer_color( aShapes[k]->GetLayer() );
        }

        p.done = [aFrame, ids]()
        {
            for( const KIID& id : ids )
                reveal( aFrame, id );
        };
        out.push_back( add( aPieces, std::move( p ) ) );
    }

    return out;
}

PIECE type_text( PCB_EDIT_FRAME* aFrame, const EDA_TEXT& aText, const BOARD_ITEM& aItem, std::vector<int> aAfter )
{
    PIECE       p;
    const BOX2I b = aItem.GetBoundingBox();
    p.kind = PIECE::TYPE;
    p.text = std::string( aText.GetShownText( true ).ToUTF8() );
    p.size = aText.GetTextHeight();
    p.pts.push_back( PT{ double( b.GetLeft() ), double( b.GetTop() ) } );
    p.color = layer_color( aItem.GetLayer() );
    p.after = std::move( aAfter );
    const KIID id = aItem.m_Uuid;
    p.done = [aFrame, id]() { reveal( aFrame, id ); };
    return p;
}

// a footprint: its outline traced, its pads put down one by one, its reference and value typed, then the rest of it
int plan_footprint( PCB_EDIT_FRAME* aFrame, std::vector<PIECE>& aPieces, FOOTPRINT* aFp, const std::vector<int>& aAfter,
                    const std::string& aTag )
{
    std::vector<PCB_SHAPE*> silk, court, fab;

    for( BOARD_ITEM* gi : aFp->GraphicalItems() )
    {
        if( gi->Type() != PCB_SHAPE_T )
            continue;

        PCB_SHAPE* s = static_cast<PCB_SHAPE*>( gi );
        const PCB_LAYER_ID l = s->GetLayer();

        if( l == F_SilkS || l == B_SilkS )
            silk.push_back( s );
        else if( l == F_CrtYd || l == B_CrtYd )
            court.push_back( s );
        else if( l == F_Fab || l == B_Fab )
            fab.push_back( s );
    }

    const std::vector<PCB_SHAPE*>& outline = !silk.empty() ? silk : !court.empty() ? court : fab;
    std::vector<int>               first;
    const int                      final = -2;   // patched below: the outline is held until the footprint shows

    if( !outline.empty() )
    {
        first = trace_shapes( aFrame, aPieces, outline, outline.size() > 12 ? 3 : 2, aAfter, final, aTag );
    }
    else
    {
        PIECE p;
        p.kind = PIECE::TRACE;
        box_points( aFp->GetBoundingBox( false ), p.pts );
        p.after = aAfter;
        p.hold = final;
        p.tag = aTag;
        first.push_back( add( aPieces, std::move( p ) ) );
    }

    // pads, row by row, a few to a robot
    std::vector<PAD*> pads( aFp->Pads().begin(), aFp->Pads().end() );
    std::sort( pads.begin(), pads.end(),
               []( PAD* a, PAD* b )
               {
                   const VECTOR2I pa = a->GetPosition(), pb = b->GetPosition();
                   return pa.y != pb.y ? pa.y < pb.y : pa.x < pb.x;
               } );
    std::vector<int> padPieces;
    const size_t     per = pads.size() > 40 ? 10 : 6;

    for( size_t i = 0; i < pads.size(); i += per )
    {
        PIECE             p;
        std::vector<KIID> ids;
        p.kind = PIECE::STAMP;
        p.after = first;
        p.color = 0xD4A84A;

        for( size_t k = i; k < std::min( pads.size(), i + per ); ++k )
        {
            const BOX2I b = pads[k]->GetBoundingBox();
            p.pts.push_back( PT{ double( b.GetLeft() ), double( b.GetTop() ) } );
            p.pts.push_back( PT{ double( b.GetRight() ), double( b.GetBottom() ) } );
            p.round.push_back( pads[k]->GetShape( PADSTACK::ALL_LAYERS ) == PAD_SHAPE::CIRCLE ? 1 : 0 );
            ids.push_back( pads[k]->m_Uuid );
        }

        p.step = [aFrame, ids]( int k )
        {
            if( k >= 0 && k < (int) ids.size() )
                reveal( aFrame, ids[k] );
        };
        padPieces.push_back( add( aPieces, std::move( p ) ) );
    }

    std::vector<int> made = padPieces.empty() ? first : padPieces;
    std::vector<int> texts;

    for( PCB_FIELD* f : { &aFp->Reference(), &aFp->Value() } )
    {
        if( f->IsVisible() && !f->GetShownText( true ).IsEmpty() )
            texts.push_back( add( aPieces, type_text( aFrame, *f, *f, made ) ) );
    }

    // the click that makes it whole: everything else of it (fab drawings, other texts) shows with it
    PIECE fin;
    fin.kind = PIECE::STAMP;
    const VECTOR2I c = aFp->GetPosition();
    const double   r = std::max( 150000.0, std::min( aFp->GetBoundingBox( false ).GetWidth(),
                                                     aFp->GetBoundingBox( false ).GetHeight() ) * 0.06 );
    fin.pts = { PT{ c.x - r, c.y - r }, PT{ c.x + r, c.y + r } };
    fin.round = { 1 };
    fin.after = made;
    fin.after.insert( fin.after.end(), texts.begin(), texts.end() );
    fin.after.insert( fin.after.end(), first.begin(), first.end() );
    const KIID id = aFp->m_Uuid;
    fin.done = [aFrame, id]() { reveal_all( aFrame, id ); };
    const int f = add( aPieces, std::move( fin ) );

    for( PIECE& p : aPieces )
        if( p.hold == final )
            p.hold = f;

    return f;
}

// tracks walked into routes (segments meeting end to end on one layer and net), each traced segment by segment
std::vector<std::vector<PCB_TRACK*>> routes( std::vector<PCB_TRACK*> aTracks )
{
    std::vector<std::vector<PCB_TRACK*>> out;
    std::vector<bool>                    used( aTracks.size(), false );

    auto joins = [&]( const VECTOR2I& p, size_t& found, bool& flip, PCB_TRACK* from ) -> bool
    {
        for( size_t j = 0; j < aTracks.size(); ++j )
        {
            if( used[j] || aTracks[j]->GetLayer() != from->GetLayer() || aTracks[j]->GetNetCode() != from->GetNetCode() )
                continue;
            if( aTracks[j]->GetStart() == p || aTracks[j]->GetEnd() == p )
            {
                found = j;
                flip = aTracks[j]->GetEnd() == p;
                return true;
            }
        }
        return false;
    };

    for( size_t i = 0; i < aTracks.size(); ++i )
    {
        if( used[i] )
            continue;

        used[i] = true;
        std::vector<PCB_TRACK*> route{ aTracks[i] };
        VECTOR2I                end = aTracks[i]->GetEnd();
        size_t                  j;
        bool                    flip;

        while( route.size() < 40 && joins( end, j, flip, route.back() ) )
        {
            used[j] = true;
            route.push_back( aTracks[j] );
            end = flip ? aTracks[j]->GetStart() : aTracks[j]->GetEnd();
        }

        out.push_back( route );
    }

    return out;
}

// the changes from a reload or the board's listeners, as one plan: the board's outline first, then the footprints,
// then the routing (tracks traced, vias put down, zones outlined), then other drawings; a changed item gets a box
void act_out( PCB_EDIT_FRAME* aFrame, const std::vector<std::pair<BOARD_ITEM*, bool>>& aWork )
{
    if( !KIPLATFORM::WORKBENCH::SwarmOn() || aWork.empty() )
        return;

    KIGFX::VIEW*             view = aFrame->GetCanvas()->GetView();
    std::vector<PIECE>       pieces;
    std::vector<PCB_SHAPE*>  edges, drawn;
    std::vector<PCB_TEXT*>   texts;
    std::vector<FOOTPRINT*>  fps;
    std::vector<PCB_TRACK*>  tracks;
    std::vector<PCB_VIA*>    vias;
    std::vector<ZONE*>       zones;
    std::vector<BOARD_ITEM*> other;
    std::set<BOARD_ITEM*>    seen;

    for( const auto& [item, isNew] : aWork )
    {
        if( !item || !seen.insert( item ).second || g_pending.count( item->m_Uuid ) )
            continue;

        if( !isNew )
        {
            // changed: a robot drags a box over it, where it now is; it shows as it is then
            const KIID id = item->m_Uuid;
            hide( view, item );
            KIPLATFORM::WORKBENCH::SwarmTargetLive(
                    [aFrame, id]( double& x, double& y, double& w, double& h ) -> int
                    {
                        BOARD_ITEM* it = aFrame->GetBoard() ? aFrame->GetBoard()->ResolveItem( id, true ) : nullptr;
                        return it ? where_box( aFrame, it->GetBoundingBox(), x, y, w, h ) : 0;
                    },
                    [aFrame, id]() { reveal( aFrame, id ); } );
            continue;
        }

        switch( item->Type() )
        {
        case PCB_FOOTPRINT_T: fps.push_back( static_cast<FOOTPRINT*>( item ) ); hide_all( view, item ); break;
        case PCB_TRACE_T:
        case PCB_ARC_T: tracks.push_back( static_cast<PCB_TRACK*>( item ) ); hide( view, item ); break;
        case PCB_VIA_T: vias.push_back( static_cast<PCB_VIA*>( item ) ); hide( view, item ); break;
        case PCB_ZONE_T: zones.push_back( static_cast<ZONE*>( item ) ); hide( view, item ); break;
        case PCB_SHAPE_T:
            ( item->GetLayer() == Edge_Cuts ? edges : drawn ).push_back( static_cast<PCB_SHAPE*>( item ) );
            hide( view, item );
            break;
        case PCB_TEXT_T: texts.push_back( static_cast<PCB_TEXT*>( item ) ); hide( view, item ); break;
        default: other.push_back( item ); hide( view, item ); break;
        }
    }

    // the board's outline
    std::vector<int> outline = trace_shapes( aFrame, pieces, edges, std::clamp<int>( (int) edges.size() / 3, 1, 4 ), {}, -1,
                                             "outline" );
    // the footprints, each by itself (several at once)
    std::vector<int> parts;
    std::sort( fps.begin(), fps.end(),
               []( FOOTPRINT* a, FOOTPRINT* b ) { return a->GetPosition().x < b->GetPosition().x; } );

    for( FOOTPRINT* fp : fps )
        parts.push_back( plan_footprint( aFrame, pieces, fp, outline,
                                         "place " + std::string( fp->GetReference().ToUTF8() ) ) );

    std::vector<int> placed = outline;
    placed.insert( placed.end(), parts.begin(), parts.end() );
    // the routing
    std::vector<int> routed;

    for( const std::vector<PCB_TRACK*>& route : routes( tracks ) )
    {
        PIECE             p;
        std::vector<KIID> ids;
        p.kind = PIECE::TRACE;
        p.after = placed;
        p.width = route.front()->GetWidth();
        p.color = layer_color( route.front()->GetLayer() );
        p.tag = routed.empty() ? std::string( "route" ) : std::string();
        VECTOR2I at = route.front()->GetStart();
        p.pts.push_back( pt( at ) );

        for( PCB_TRACK* t : route )
        {
            const bool     flip = t->GetEnd() == at && t->GetStart() != at;
            const VECTOR2I to = flip ? t->GetStart() : t->GetEnd();

            if( t->Type() == PCB_ARC_T )
            {
                PCB_ARC*               a = static_cast<PCB_ARC*>( t );
                const SHAPE_LINE_CHAIN ch = SHAPE_ARC( at, a->GetMid(), to, 0 ).ConvertToPolyline();
                for( int i = 1; i + 1 < ch.PointCount(); ++i )
                    p.pts.push_back( pt( ch.CPoint( i ) ) );
            }

            p.pts.push_back( pt( to ) );
            ids.push_back( t->m_Uuid );
            at = to;
        }

        // the steps are the polyline's segments: a segment's track shows once the pen has passed its end
        std::vector<int> owner;   // per segment, the track it ends
        VECTOR2I         cur = route.front()->GetStart();

        for( size_t k = 0; k < route.size(); ++k )
        {
            PCB_TRACK*     t = route[k];
            const bool     flip = t->GetEnd() == cur && t->GetStart() != cur;
            const VECTOR2I to = flip ? t->GetStart() : t->GetEnd();
            int            segs = 1;

            if( t->Type() == PCB_ARC_T )
                segs = std::max( 1, SHAPE_ARC( cur, static_cast<PCB_ARC*>( t )->GetMid(), to, 0 ).ConvertToPolyline().PointCount() - 1 );

            for( int s = 0; s < segs; ++s )
                owner.push_back( s == segs - 1 ? int( k ) : -1 );

            cur = to;
        }

        p.step = [aFrame, ids, owner]( int k )
        {
            if( k >= 0 && k < (int) owner.size() && owner[k] >= 0 )
                reveal( aFrame, ids[owner[k]] );
        };
        routed.push_back( add( pieces, std::move( p ) ) );
    }

    std::vector<int> wired = placed;
    wired.insert( wired.end(), routed.begin(), routed.end() );
    std::sort( vias.begin(), vias.end(), []( PCB_VIA* a, PCB_VIA* b ) { return a->GetPosition().x < b->GetPosition().x; } );

    for( size_t i = 0; i < vias.size(); i += 6 )
    {
        PIECE             p;
        std::vector<KIID> ids;
        p.kind = PIECE::STAMP;
        p.after = wired;
        p.color = 0xBDBDBD;

        for( size_t k = i; k < std::min( vias.size(), i + 6 ); ++k )
        {
            const VECTOR2I c = vias[k]->GetPosition();
            const double   r = vias[k]->GetWidth( F_Cu ) / 2.0;
            p.pts.push_back( PT{ c.x - r, c.y - r } );
            p.pts.push_back( PT{ c.x + r, c.y + r } );
            p.round.push_back( 1 );
            ids.push_back( vias[k]->m_Uuid );
        }

        p.step = [aFrame, ids]( int k )
        {
            if( k >= 0 && k < (int) ids.size() )
                reveal( aFrame, ids[k] );
        };
        add( pieces, std::move( p ) );
    }

    for( ZONE* z : zones )
    {
        PIECE p;
        p.kind = PIECE::TRACE;
        p.after = wired;
        p.color = layer_color( z->GetFirstLayer() );
        p.tag = "zone";
        const SHAPE_POLY_SET* poly = z->Outline();

        for( int o = 0; poly && o < poly->OutlineCount(); ++o )
        {
            const SHAPE_LINE_CHAIN& ch = poly->COutline( o );
            if( !p.pts.empty() )
                p.pts.push_back( LIFT );
            for( int i = 0; i < ch.PointCount(); ++i )
                p.pts.push_back( pt( ch.CPoint( i ) ) );
            if( ch.PointCount() )
                p.pts.push_back( pt( ch.CPoint( 0 ) ) );
        }

        const KIID id = z->m_Uuid;
        p.done = [aFrame, id]() { reveal( aFrame, id ); };
        add( pieces, std::move( p ) );
    }

    trace_shapes( aFrame, pieces, drawn, std::clamp<int>( (int) drawn.size() / 3, 1, 6 ), placed, -1, std::string() );

    for( PCB_TEXT* t : texts )
        add( pieces, type_text( aFrame, *t, *t, placed ) );

    for( BOARD_ITEM* item : other )
    {
        PIECE p;
        p.kind = PIECE::TRACE;
        box_points( item->GetBoundingBox(), p.pts );
        p.after = placed;
        const KIID id = item->m_Uuid;
        p.done = [aFrame, id]() { reveal_all( aFrame, id ); };
        add( pieces, std::move( p ) );
    }

    if( !pieces.empty() )
        KIPLATFORM::WORKBENCH::SwarmPlan( std::move( pieces ), view_map( aFrame ) );

    aFrame->GetCanvas()->Refresh();
}

class WORKBENCH_BOARD_LISTENER : public BOARD_LISTENER
{
public:
    explicit WORKBENCH_BOARD_LISTENER( PCB_EDIT_FRAME* aFrame ) : m_frame( aFrame ) {}

    void SetFrame( PCB_EDIT_FRAME* aFrame ) { m_frame = aFrame; }   // a frame made again (after another project opened)

    void OnBoardItemAdded( BOARD&, BOARD_ITEM* aItem ) override { act_out( m_frame, { { aItem, true } } ); }
    void OnBoardItemsAdded( BOARD&, std::vector<BOARD_ITEM*>& aItems ) override { act_out( m_frame, batch( aItems, true ) ); }
    void OnBoardItemChanged( BOARD&, BOARD_ITEM* aItem ) override { act_out( m_frame, { { aItem, false } } ); }
    void OnBoardItemsChanged( BOARD&, std::vector<BOARD_ITEM*>& aItems ) override { act_out( m_frame, batch( aItems, false ) ); }

private:
    static std::vector<std::pair<BOARD_ITEM*, bool>> batch( std::vector<BOARD_ITEM*>& aItems, bool aNew )
    {
        std::vector<std::pair<BOARD_ITEM*, bool>> out;
        for( BOARD_ITEM* item : aItems )
            out.emplace_back( item, aNew );
        return out;
    }

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
    if( !KIPLATFORM::WORKBENCH::Docked() || !aFrame->GetBoard() )
        return;

    if( g_listener )
        g_listener->SetFrame( aFrame );

    if( g_watched == aFrame->GetBoard() )
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
            if( !aFrame->SaveBoard() )
                throw std::runtime_error( "couldn't save the board" );

            result = { { "saved", std::string( aFrame->GetBoard()->GetFileName().ToUTF8() ) } };
        }
        else if( cmd == "reload" )
        {
            // the user's own zoom and pan (once they've moved the view) are kept through the reload
            const bool     userView = user_moved( aFrame );
            const VECTOR2D keepCenter = aFrame->GetCanvas()->GetView()->GetCenter();
            const double   keepScale = aFrame->GetCanvas()->GetView()->GetScale();
            // the board as Claude left it on disk; what changed gets a robot (new items drawn out by theirs)
            const std::map<KIID, BOX2I> before = snapshot( aFrame->GetBoard() );
            const wxString              file = aFrame->GetBoard()->GetFileName();

            aFrame->GetScreen()->SetContentModified( false );   // the file on disk wins: nothing to ask about
            if( !aFrame->OpenProjectFiles( std::vector<wxString>{ file }, KICTL_REVERT ) )
                throw std::runtime_error( "couldn't reload the board" );

            if( userView )
            {
                aFrame->GetCanvas()->GetView()->SetScale( keepScale );
                aFrame->GetCanvas()->GetView()->SetCenter( keepCenter );
            }

            WorkbenchPcbWatch( aFrame );
            BOARD* board = aFrame->GetBoard();
            int    added = 0, changed = 0;

            std::vector<std::pair<KIID, bool>> work;   // what changed, and whether it's new
            BOX2I                              where;

            for( const auto& [id, box] : snapshot( board ) )
            {
                auto it = before.find( id );

                if( it == before.end() || it->second != box )
                {
                    work.emplace_back( id, it == before.end() );
                    where.Merge( box );
                    ++( it == before.end() ? added : changed );
                }
            }

            // the robots act it out where it can be seen: the view goes to the new work when it's outside the
            // view or small in it (the work about two thirds of the view; the board's frame isn't what's being built)
            if( !work.empty() && KIPLATFORM::WORKBENCH::SwarmOn() && !userView )
            {
                KIGFX::VIEW* v = aFrame->GetCanvas()->GetView();
                const BOX2D  view = v->GetViewport();
                const BOX2D  w( VECTOR2D( where.GetOrigin() ), VECTOR2D( where.GetSize() ) );
                const double fill = std::max( w.GetWidth() / view.GetWidth(), w.GetHeight() / view.GetHeight() );

                if( !view.Contains( w ) || fill < 0.35 )
                {
                    double       wd = std::max( w.GetWidth() * 1.5, 8e6 );
                    double       ht = std::max( w.GetHeight() * 1.5, 8e6 * 0.6 );
                    const double aspect = view.GetHeight() > 0 ? view.GetWidth() / view.GetHeight() : 1.0;

                    if( wd / ht < aspect )
                        wd = ht * aspect;
                    else
                        ht = wd / aspect;

                    v->SetViewport( BOX2D( w.Centre() - VECTOR2D( wd / 2, ht / 2 ), VECTOR2D( wd, ht ) ) );
                    aFrame->GetCanvas()->Refresh();
                }
            }

            hide_pending( aFrame );
            std::vector<std::pair<BOARD_ITEM*, bool>> items;

            for( const auto& [id, isNew] : work )
                items.emplace_back( board->ResolveItem( id, true ), isNew );

            act_out( aFrame, items );

            if( !userView )
                mark_view( aFrame );   // the view as Workbench left it

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
