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
#include <geometry/shape_arc.h>
#include <geometry/shape_line_chain.h>
#include <lib_symbol.h>
#include <sch_field.h>
#include <sch_label.h>
#include <sch_line.h>
#include <sch_pin.h>
#include <sch_shape.h>
#include <sch_sheet.h>
#include <sch_symbol.h>
#include <sch_text.h>
#include <set>
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

// ---------------------------------------------------------------- the build plan

using PIECE = KIPLATFORM::WORKBENCH::SWARM_PIECE;
using PT = KIPLATFORM::WORKBENCH::SWARM_PT;

// what the swarm keeps out of sight until its robot has made it (a reload makes the sheet again: hidden again then)
std::set<KIID> g_pending;

// the sheet's world (internal units) to the overlay (points, from the docked window's content top-left), this frame
KIPLATFORM::WORKBENCH::SWARM_VIEW view_map( SCH_EDIT_FRAME* aFrame )
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
        const VECTOR2D ex = view->ToScreen( VECTOR2D( 1e5, 0 ) ) - o;
        const VECTOR2D ey = view->ToScreen( VECTOR2D( 0, 1e5 ) ) - o;
        m[0] = ex.x / 1e5 / scale;
        m[1] = ey.x / 1e5 / scale;
        m[2] = origin.x + o.x / scale;
        m[3] = ex.y / 1e5 / scale;
        m[4] = ey.y / 1e5 / scale;
        m[5] = origin.y + o.y / scale;
        return true;
    };
}

void hide( KIGFX::VIEW* aView, SCH_ITEM* aItem )
{
    aView->Hide( aItem, true );
    g_pending.insert( aItem->m_Uuid );
}

void reveal( SCH_EDIT_FRAME* aFrame, const KIID& aId )
{
    g_pending.erase( aId );

    if( SCH_ITEM* item = aFrame->Schematic().ResolveItem( aId, nullptr, true ) )
    {
        aFrame->GetCanvas()->GetView()->Hide( item, false );
        aFrame->GetCanvas()->Refresh();
    }
}

// after a reload (the sheet made again from the file): what the robots haven't made yet stays out of sight
void hide_pending( SCH_EDIT_FRAME* aFrame )
{
    for( auto it = g_pending.begin(); it != g_pending.end(); )
    {
        if( SCH_ITEM* item = aFrame->Schematic().ResolveItem( *it, nullptr, true ) )
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

const PT LIFT{ NAN, NAN };

PT pt( const VECTOR2I& p )
{
    return PT{ double( p.x ), double( p.y ) };
}

void box_points( const BOX2I& b, std::vector<PT>& out )
{
    out.push_back( PT{ double( b.GetLeft() ), double( b.GetTop() ) } );
    out.push_back( PT{ double( b.GetRight() ), double( b.GetTop() ) } );
    out.push_back( PT{ double( b.GetRight() ), double( b.GetBottom() ) } );
    out.push_back( PT{ double( b.GetLeft() ), double( b.GetBottom() ) } );
    out.push_back( PT{ double( b.GetLeft() ), double( b.GetTop() ) } );
}

// a library shape's line as points (one stroke, or several with lifts), placed by the symbol's transform
void shape_points( const EDA_SHAPE& s, const std::function<VECTOR2I( const VECTOR2I& )>& at, std::vector<PT>& out )
{
    auto put = [&]( const VECTOR2I& p ) { out.push_back( pt( at( p ) ) ); };

    switch( s.GetShape() )
    {
    case SHAPE_T::SEGMENT:
        put( s.GetStart() );
        put( s.GetEnd() );
        break;
    case SHAPE_T::RECTANGLE:
    {
        const std::vector<VECTOR2I> c = s.GetRectCorners();
        for( const VECTOR2I& p : c )
            put( p );
        if( !c.empty() )
            put( c.front() );
        break;
    }
    case SHAPE_T::CIRCLE:
    {
        const VECTOR2I c = s.GetStart();   // a circle: its centre
        const double   r = s.GetRadius();
        for( int i = 0; i <= 20; ++i )
            put( VECTOR2I( KiROUND( c.x + r * std::cos( i * M_PI / 10 ) ), KiROUND( c.y + r * std::sin( i * M_PI / 10 ) ) ) );
        break;
    }
    case SHAPE_T::ARC:
    {
        const SHAPE_LINE_CHAIN ch = SHAPE_ARC( s.GetStart(), s.GetArcMid(), s.GetEnd(), 0 ).ConvertToPolyline( 2000 );
        for( int i = 0; i < ch.PointCount(); ++i )
            put( ch.CPoint( i ) );
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
                put( ch.CPoint( i ) );
            if( ch.IsClosed() && ch.PointCount() )
                put( ch.CPoint( 0 ) );
        }
        break;
    }
    case SHAPE_T::BEZIER:
        for( const VECTOR2I& p : s.GetBezierPoints() )
            put( p );
        break;
    default: break;
    }
}

int add( std::vector<PIECE>& aPieces, PIECE aPiece )
{
    aPieces.push_back( std::move( aPiece ) );
    return int( aPieces.size() ) - 1;
}

PIECE type_text( SCH_EDIT_FRAME* aFrame, const wxString& aText, int aHeight, const BOX2I& aBox, std::vector<int> aAfter,
                 const KIID* aReveal, unsigned aColor = 0 )
{
    PIECE p;
    p.kind = PIECE::TYPE;
    p.text = std::string( aText.ToUTF8() );
    p.size = aHeight;
    p.pts.push_back( PT{ double( aBox.GetLeft() ), double( aBox.GetTop() ) } );
    p.after = std::move( aAfter );
    p.color = aColor;

    if( aReveal )
    {
        const KIID id = *aReveal;
        p.done = [aFrame, id]() { reveal( aFrame, id ); };
    }

    return p;
}

// a symbol: its body outline traced, then its pins one by one, then its reference and value typed, then it shows
int plan_symbol( SCH_EDIT_FRAME* aFrame, std::vector<PIECE>& aPieces, SCH_SYMBOL* aSym, const std::vector<int>& aAfter )
{
    const int        hold = -2;   // patched below: the drawing stays until the symbol shows
    const TRANSFORM& t = aSym->GetTransform();
    const VECTOR2I   pos = aSym->GetPosition();
    auto             at = [&]( const VECTOR2I& p ) { return t.TransformCoordinate( p ) + pos; };
    std::vector<int> body;
    std::vector<PT>  strokes;
    int              shapes = 0;

    if( const std::unique_ptr<LIB_SYMBOL>& lib = aSym->GetLibSymbolRef() )
    {
        for( const SCH_ITEM& item : lib->GetDrawItems()[SCH_SHAPE_T] )
        {
            if( item.GetUnit() && item.GetUnit() != aSym->GetUnit() )
                continue;
            if( item.GetBodyStyle() && item.GetBodyStyle() != aSym->GetBodyStyle() )
                continue;

            if( !strokes.empty() )
                strokes.push_back( LIFT );

            shape_points( static_cast<const SCH_SHAPE&>( item ), at, strokes );
            ++shapes;
        }
    }

    if( strokes.empty() )
        box_points( aSym->GetBodyBoundingBox(), strokes );

    {
        PIECE p;
        p.kind = PIECE::TRACE;
        p.pts = std::move( strokes );
        p.after = aAfter;
        p.hold = hold;
        p.tag = "place " + std::string( aSym->GetRef( &aFrame->GetCurrentSheet() ).ToUTF8() );
        body.push_back( add( aPieces, std::move( p ) ) );
    }

    // pins, each from the body out to where a wire meets it
    std::vector<int> made = body;
    std::vector<SCH_PIN*> pins = aSym->GetPins( &aFrame->GetCurrentSheet() );

    if( !pins.empty() )
    {
        PIECE p;
        p.kind = PIECE::TRACE;
        p.after = body;
        p.hold = hold;

        for( SCH_PIN* pin : pins )
        {
            if( !pin->IsVisible() )
                continue;
            if( !p.pts.empty() )
                p.pts.push_back( LIFT );
            p.pts.push_back( pt( pin->GetPinRoot() ) );
            p.pts.push_back( pt( pin->GetPosition() ) );
        }

        if( !p.pts.empty() )
            made = { add( aPieces, std::move( p ) ) };
    }

    std::vector<int> fields;

    for( const SCH_FIELD& f : aSym->GetFields() )
    {
        if( !f.IsVisible() || ( f.GetId() != FIELD_T::REFERENCE && f.GetId() != FIELD_T::VALUE ) )
            continue;

        const wxString text = f.GetShownText( &aFrame->GetCurrentSheet(), false );

        if( text.IsEmpty() )
            continue;

        PIECE p = type_text( aFrame, text, f.GetTextHeight(), f.GetBoundingBox(), made, nullptr );
        p.hold = hold;
        fields.push_back( add( aPieces, std::move( p ) ) );
    }

    // the click that makes it real
    PIECE fin;
    fin.kind = PIECE::STAMP;
    const double r = std::max( 2000.0, std::min( aSym->GetBodyBoundingBox().GetWidth(), aSym->GetBodyBoundingBox().GetHeight() ) * 0.06 );
    const VECTOR2I c = aSym->GetBodyBoundingBox().Centre();
    fin.pts = { PT{ c.x - r, c.y - r }, PT{ c.x + r, c.y + r } };
    fin.round = { 1 };
    fin.after = made;
    fin.after.insert( fin.after.end(), fields.begin(), fields.end() );
    fin.after.insert( fin.after.end(), body.begin(), body.end() );
    const KIID id = aSym->m_Uuid;
    fin.done = [aFrame, id]() { reveal( aFrame, id ); };
    const int f = add( aPieces, std::move( fin ) );

    for( PIECE& p : aPieces )
        if( p.hold == hold )
            p.hold = f;

    (void) shapes;
    return f;
}

// the changes from a reload or the schematic's listeners, as one plan: sheets and symbols first (several at once),
// then the wires traced segment by segment, then junctions, no-connects and labels; a changed item gets a box
void act_out( SCH_EDIT_FRAME* aFrame, const std::vector<std::pair<SCH_ITEM*, bool>>& aWork )
{
    if( !KIPLATFORM::WORKBENCH::SwarmOn() || aWork.empty() )
        return;

    KIGFX::VIEW*            view = aFrame->GetCanvas()->GetView();
    std::vector<PIECE>      pieces;
    std::vector<SCH_SYMBOL*> syms;
    std::vector<SCH_SHEET*> sheets;
    std::vector<SCH_LINE*>  lines;
    std::vector<SCH_ITEM*>  dots, texts, other;
    std::set<SCH_ITEM*>     seen;

    for( const auto& [item, isNew] : aWork )
    {
        if( !item || !on_sheet_shown( aFrame, item ) || !seen.insert( item ).second || g_pending.count( item->m_Uuid ) )
            continue;

        if( !isNew )
        {
            // changed: a robot drags a box over it, where it now is; it shows as it is then
            const KIID id = item->m_Uuid;
            hide( view, item );
            KIPLATFORM::WORKBENCH::SwarmTargetLive(
                    [aFrame, id]( double& x, double& y, double& w, double& h ) -> int
                    {
                        SCH_ITEM* it = aFrame->Schematic().ResolveItem( id, nullptr, true );
                        return it ? where_box( aFrame, it->GetBoundingBox(), x, y, w, h ) : 0;
                    },
                    [aFrame, id]() { reveal( aFrame, id ); } );
            continue;
        }

        hide( view, item );

        switch( item->Type() )
        {
        case SCH_SYMBOL_T: syms.push_back( static_cast<SCH_SYMBOL*>( item ) ); break;
        case SCH_SHEET_T: sheets.push_back( static_cast<SCH_SHEET*>( item ) ); break;
        case SCH_LINE_T: lines.push_back( static_cast<SCH_LINE*>( item ) ); break;
        case SCH_JUNCTION_T:
        case SCH_NO_CONNECT_T: dots.push_back( item ); break;
        case SCH_LABEL_T:
        case SCH_GLOBAL_LABEL_T:
        case SCH_HIER_LABEL_T:
        case SCH_DIRECTIVE_LABEL_T:
        case SCH_TEXT_T: texts.push_back( item ); break;
        default: other.push_back( item ); break;
        }
    }

    std::vector<int> placed;

    for( SCH_SHEET* sh : sheets )
    {
        PIECE p;
        p.kind = PIECE::TRACE;
        box_points( BOX2I( sh->GetPosition(), sh->GetSize() ), p.pts );
        p.tag = "sheet";
        const KIID id = sh->m_Uuid;
        p.done = [aFrame, id]() { reveal( aFrame, id ); };
        placed.push_back( add( pieces, std::move( p ) ) );
    }

    std::sort( syms.begin(), syms.end(), []( SCH_SYMBOL* a, SCH_SYMBOL* b ) { return a->GetPosition().x < b->GetPosition().x; } );

    for( SCH_SYMBOL* sym : syms )
        placed.push_back( plan_symbol( aFrame, pieces, sym, {} ) );

    // wires walked into runs (segments meeting end to end), each traced segment by segment
    std::vector<bool> used( lines.size(), false );
    std::vector<int>  wired = placed;
    bool              first = true;

    for( size_t i = 0; i < lines.size(); ++i )
    {
        if( used[i] )
            continue;

        used[i] = true;
        std::vector<SCH_LINE*> run{ lines[i] };
        std::vector<VECTOR2I>  pts{ lines[i]->GetStartPoint(), lines[i]->GetEndPoint() };

        for( bool grew = true; grew && run.size() < 40; )
        {
            grew = false;

            for( size_t j = 0; j < lines.size(); ++j )
            {
                if( used[j] || lines[j]->GetLayer() != lines[i]->GetLayer() )
                    continue;

                if( lines[j]->GetStartPoint() == pts.back() || lines[j]->GetEndPoint() == pts.back() )
                {
                    used[j] = true;
                    run.push_back( lines[j] );
                    pts.push_back( lines[j]->GetStartPoint() == pts.back() ? lines[j]->GetEndPoint() : lines[j]->GetStartPoint() );
                    grew = true;
                    break;
                }
            }
        }

        PIECE             p;
        std::vector<KIID> ids;
        p.kind = PIECE::TRACE;
        p.after = placed;
        p.color = lines[i]->IsBus() ? 0x2F80ED : lines[i]->IsWire() ? 0x4CAF50 : 0;
        p.width = lines[i]->GetPenWidth();
        p.tag = first ? std::string( "wire" ) : std::string();
        first = false;

        for( const VECTOR2I& q : pts )
            p.pts.push_back( pt( q ) );

        for( SCH_LINE* l : run )
            ids.push_back( l->m_Uuid );

        p.step = [aFrame, ids]( int k )
        {
            if( k >= 0 && k < (int) ids.size() )
                reveal( aFrame, ids[k] );
        };
        wired.push_back( add( pieces, std::move( p ) ) );
    }

    std::sort( dots.begin(), dots.end(), []( SCH_ITEM* a, SCH_ITEM* b ) { return a->GetPosition().x < b->GetPosition().x; } );

    for( size_t i = 0; i < dots.size(); i += 6 )
    {
        PIECE             p;
        std::vector<KIID> ids;
        p.kind = PIECE::STAMP;
        p.after = wired;
        p.color = 0x4CAF50;

        for( size_t k = i; k < std::min( dots.size(), i + 6 ); ++k )
        {
            const BOX2I b = dots[k]->GetBoundingBox();
            p.pts.push_back( PT{ double( b.GetLeft() ), double( b.GetTop() ) } );
            p.pts.push_back( PT{ double( b.GetRight() ), double( b.GetBottom() ) } );
            p.round.push_back( dots[k]->Type() == SCH_JUNCTION_T ? 1 : 0 );
            ids.push_back( dots[k]->m_Uuid );
        }

        p.step = [aFrame, ids]( int k )
        {
            if( k >= 0 && k < (int) ids.size() )
                reveal( aFrame, ids[k] );
        };
        add( pieces, std::move( p ) );
    }

    for( SCH_ITEM* item : texts )
    {
        const SCH_TEXT* t = dynamic_cast<const SCH_TEXT*>( item );
        const KIID      id = item->m_Uuid;

        if( t )
        {
            PIECE p = type_text( aFrame, t->GetShownText( &aFrame->GetCurrentSheet(), false ), t->GetTextHeight(),
                                 item->GetBoundingBox(), wired, &id );
            add( pieces, std::move( p ) );
        }
        else
        {
            other.push_back( item );
        }
    }

    for( SCH_ITEM* item : other )
    {
        PIECE p;
        p.kind = PIECE::TRACE;
        box_points( item->GetBoundingBox(), p.pts );
        p.after = placed;
        const KIID id = item->m_Uuid;
        p.done = [aFrame, id]() { reveal( aFrame, id ); };
        add( pieces, std::move( p ) );
    }

    if( !pieces.empty() )
        KIPLATFORM::WORKBENCH::SwarmPlan( std::move( pieces ), view_map( aFrame ) );

    aFrame->GetCanvas()->Refresh();
}

class WORKBENCH_SCH_LISTENER : public SCHEMATIC_LISTENER
{
public:
    explicit WORKBENCH_SCH_LISTENER( SCH_EDIT_FRAME* aFrame ) : m_frame( aFrame ) {}

    void SetFrame( SCH_EDIT_FRAME* aFrame ) { m_frame = aFrame; }   // a frame made again (after another project opened)

    void OnSchItemsAdded( SCHEMATIC&, std::vector<SCH_ITEM*>& aItems ) override { act_out( m_frame, batch( aItems, true ) ); }
    void OnSchItemsChanged( SCHEMATIC&, std::vector<SCH_ITEM*>& aItems ) override { act_out( m_frame, batch( aItems, false ) ); }

private:
    static std::vector<std::pair<SCH_ITEM*, bool>> batch( std::vector<SCH_ITEM*>& aItems, bool aNew )
    {
        std::vector<std::pair<SCH_ITEM*, bool>> out;
        for( SCH_ITEM* item : aItems )
            out.emplace_back( item, aNew );
        return out;
    }

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

            // the robots act it out where it can be seen: the view goes to the new work when it's outside the
            // view or small in it (the work about two thirds of the view; the sheet's frame isn't what's being built)
            if( !work.empty() && KIPLATFORM::WORKBENCH::SwarmOn() && !userView )
            {
                KIGFX::VIEW* v = aFrame->GetCanvas()->GetView();
                const BOX2D  view = v->GetViewport();
                const BOX2D  w( VECTOR2D( where.GetOrigin() ), VECTOR2D( where.GetSize() ) );
                const double fill = std::max( w.GetWidth() / view.GetWidth(), w.GetHeight() / view.GetHeight() );

                if( !view.Contains( w ) || fill < 0.35 )
                {
                    double       wd = std::max( w.GetWidth() * 1.5, 300000.0 );
                    double       ht = std::max( w.GetHeight() * 1.5, 300000.0 * 0.6 );
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
            std::vector<std::pair<SCH_ITEM*, bool>> items;

            for( const auto& [id, isNew] : work )
                items.emplace_back( aFrame->Schematic().ResolveItem( id, nullptr, true ), isNew );

            act_out( aFrame, items );

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
