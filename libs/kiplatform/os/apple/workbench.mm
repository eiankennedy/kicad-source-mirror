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

#include <kiplatform/workbench.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <list>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include <wx/dialog.h>
#include <wx/progdlg.h>
#include <wx/toplevel.h>
#include <wx/window.h>

namespace
{
// ---------------------------------------------------------------- the swarm
// Each change is a plan of small pieces, worked by a pool of robots the way the browser swarm rebuilds a page: a
// piece starts once the pieces it waits for are done, a free robot takes the nearest ready piece, more robots join
// while there's more work than hands, and each part appears as its robot finishes it (the editor shows the real item
// then). Pieces: a pen trace along polylines (an outline, a pin, a wire, a track), stamps (pads, vias, junctions),
// typed text (references, values, labels), and a box dragged over a changed item.

using PIECE = KIPLATFORM::WORKBENCH::SWARM_PIECE;
using PT = KIPLATFORM::WORKBENCH::SWARM_PT;

constexpr size_t BASE = 4, MAX_BOTS = 14;

// a trace's segments, pen up between its strokes
struct Seg
{
    PT   a, b;
    bool pen = true;
    int  step = -1;   // the pen segment's number (for the piece's step callback)
};

struct Piece
{
    PIECE            spec;
    int              state = 0;   // 0 waiting, 1 being worked, 2 done
    int              steps = 0;   // segments drawn / shapes stamped / characters typed, all told
    int              at = 0;      // steps done
    double           frac = 0;    // of the one in hand
    double           done_at = 0;
    std::vector<Seg> segs;        // a trace's
    NSString*        str = nil;   // typed text
    CAShapeLayer*    shape = nil;
    CATextLayer*     text = nil;
    std::function<int( double&, double&, double&, double& )> where;   // a box: where its item is now (overlay)
    NSRect           rect = NSZeroRect;
};

struct Plan
{
    long                              id = 0;
    std::vector<Piece>                pieces;
    KIPLATFORM::WORKBENCH::SWARM_VIEW view;
    double                            m[6] = { 1, 0, 0, 0, 1, 0 };
    std::function<void()>             done;
    bool                              finished = false;
    double                            finished_at = 0;
};

struct Bot
{
    CALayer*      layer = nil;
    CAShapeLayer* arrow = nil;   // working out of view: at the view's edge, an arrow toward the work
    CATextLayer*  tag = nil;
    NSSize        tagSize = NSZeroSize;
    double        tag_at = 0;
    NSPoint       pos = NSZeroPoint;
    NSPoint       from = NSZeroPoint;
    long          plan = -1;
    int           piece = -1;
    int           phase = 0;   // 0 flying to the work, 1 working, 2 a moment after
    double        t = 0, dur = 0, bend = 0;
    double        step_t = 0, step_dur = 0;
    bool          leaving = false;
    double        idle = 0;
    double        press = 0;
    bool          off = false;
    NSColor*      color = nil;
};

struct Swarm
{
    bool              on = false;
    NSColor*          accent = nil;
    std::string       label;
    std::vector<Bot>  bots;
    std::list<Plan>   plans;
    long              next = 1;
    double            last = 0, last_work = 0, cool = 0;
    unsigned          seed = 7;
    NSWindow*         overlay = nil;
    CALayer*          root = nil;
    NSTimer*          timer = nil;
};

// KiCad's executable and each editor (a kiface, a library of its own) link a copy of this file: they share one
// state, kept where every copy finds it, so the editors' changes reach the swarm and the dialogs know a command runs
struct Shared
{
    NSWindow*                                        dock = nil;    // the editor window over Workbench's panel
    NSDictionary*                                    place = nil;   // the last placement
    std::function<std::string( const std::string& )> handler;
    int                                              busy = 0;      // a command running now: no dialog may wait
    KIPLATFORM::WORKBENCH::UNSAVED                   answer = KIPLATFORM::WORKBENCH::UNSAVED::CANCEL;
    std::vector<std::string>                         asked;
    NSTimer*                                         watch = nil;
    Swarm                                            swarm;
};

Shared& shared()
{
    static Shared* mine = nullptr;
    if( !mine )
    {
        NSMutableDictionary* registry = NSThread.mainThread.threadDictionary;
        NSValue* v = registry[@"KiCadWorkbenchDock"];
        if( !v )
        {
            v = [NSValue valueWithPointer:new Shared()];
            registry[@"KiCadWorkbenchDock"] = v;
        }
        mine = static_cast<Shared*>( v.pointerValue );
    }
    return *mine;
}

#define g_dock ( shared().dock )
#define g_place ( shared().place )
#define g_handler ( shared().handler )
#define g_busy ( shared().busy )
#define g_answer ( shared().answer )
#define g_asked ( shared().asked )
#define g_watch ( shared().watch )
#define S ( shared().swarm )


void place( NSDictionary* msg );
void swarm_start();

// ---------------------------------------------------------------- connection

void write_line( int fd, NSDictionary* msg )
{
    if( fd < 0 )
        return;
    NSData* data = [NSJSONSerialization dataWithJSONObject:msg options:0 error:nil];
    if( !data )
        return;
    NSMutableData* line = [data mutableCopy];
    [line appendBytes:"\n" length:1];
    (void) write( fd, line.bytes, line.length );
}

void place( NSDictionary* msg )
{
    if( !msg[@"rect"] && !msg[@"visible"] && !msg[@"above"] )
    {
        // only the focus (the user clicked the panel): the placement stays
    }
    else if( msg[@"focus"] ) // remembered without it: placing again later mustn't take the focus again
    {
        NSMutableDictionary* m = [msg mutableCopy];
        [m removeObjectForKey:@"focus"];
        g_place = m;
    }
    else
    {
        g_place = msg;
    }
    NSWindow* window = g_dock;
    if( !window )
        return;
    NSNumber* visible = msg[@"visible"];
    if( visible && !visible.boolValue )
    {
        [window orderOut:nil];
        return;
    }
    NSArray* rect = msg[@"rect"];
    if( [rect isKindOfClass:[NSArray class]] && rect.count == 4 )
    {
        NSRect frame = NSMakeRect( [rect[0] doubleValue], [rect[1] doubleValue], [rect[2] doubleValue],
                                   [rect[3] doubleValue] );
        if( frame.size.width >= 1 && frame.size.height >= 1 && !NSEqualRects( frame, window.frame ) )
            [window setFrame:frame display:YES];
    }
    NSNumber* above = msg[@"above"];
    if( above )
        [window orderWindow:NSWindowAbove relativeTo:above.integerValue];
    else if( !window.isVisible )
        [window orderFront:nil];
    if( [msg[@"focus"] boolValue] )
    {
        [NSApp activateIgnoringOtherApps:YES];
        [window makeKeyWindow];
    }
}

NSString* ns( const std::string& aText )
{
    return [NSString stringWithUTF8String:aText.c_str()] ?: @"";
}

// The last resort while a command runs: a modal dialog that KiCad opened anyway (not through confirm.cpp or
// DIALOG_SHIM) is cancelled at once, and the command's reply says what it asked.
void dismiss_modals()
{
    if( !g_busy )
        return;
    for( wxWindow* w : wxTopLevelWindows )
    {
        wxDialog* dlg = dynamic_cast<wxDialog*>( w );
        // a progress dialog (loading a file) asks nothing: it goes away by itself
        if( dlg && dlg->IsModal() && !dynamic_cast<wxGenericProgressDialog*>( dlg ) )
        {
            g_asked.push_back( std::string( dlg->GetTitle().ToUTF8() ) );
            dlg->EndModal( wxID_CANCEL );
            return;
        }
    }
    // a native alert (wxMessageDialog); not a modal session of another kind (a progress dialog's, while loading)
    NSWindow* modal = NSApp.modalWindow;
    if( modal && [NSStringFromClass( [modal class] ) containsString:@"Alert"] )
    {
        std::string text = modal.title.UTF8String ?: "";
        for( NSView* v in modal.contentView.subviews )
            if( [v isKindOfClass:[NSTextField class]] && ( (NSTextField*) v ).stringValue.length )
            {
                text = ( (NSTextField*) v ).stringValue.UTF8String;
                break;
            }
        g_asked.push_back( text.empty() ? std::string( "a dialog" ) : text );
        [NSApp abortModal];
    }
}

// Over Workbench's window again, now and once KiCad has settled: a window of its own closing (a progress dialog
// after loading, which wxWidgets destroys later) or a frame just made (which wxWidgets orders itself) can leave the
// docked window behind Workbench's
void settle()
{
    if( !g_dock || !g_place )
        return;
    place( g_place );
    NSWindow* window = g_dock;
    for( double delay : { 0.1, 0.4, 1.0 } )
    {
        dispatch_after( dispatch_time( DISPATCH_TIME_NOW, int64_t( delay * NSEC_PER_SEC ) ), dispatch_get_main_queue(),
                        ^{
                            if( g_dock == window && g_place )
                                place( g_place );
                        } );
    }
}

void command( int fd, NSDictionary* msg )
{
    id cid = msg[@"id"] ?: [NSNull null];
    NSData* data = [NSJSONSerialization dataWithJSONObject:msg options:0 error:nil];
    std::string request( static_cast<const char*>( data.bytes ), data.length );

    if( g_busy++ == 0 )
    {
        g_answer = KIPLATFORM::WORKBENCH::UNSAVED::CANCEL;
        g_asked.clear();
        // in the common modes, so it fires inside a modal dialog's run loop too
        g_watch = [NSTimer timerWithTimeInterval:0.25 repeats:YES block:^( NSTimer* ) { dismiss_modals(); }];
        [[NSRunLoop mainRunLoop] addTimer:g_watch forMode:NSRunLoopCommonModes];
    }

    NSDictionary* reply = nil;
    try
    {
        std::string result = g_handler ? g_handler( request ) : std::string( "null" );
        NSData* rd = [NSData dataWithBytes:result.data() length:result.size()];
        id r = [NSJSONSerialization JSONObjectWithData:rd options:NSJSONReadingFragmentsAllowed error:nil];
        if( !g_asked.empty() && [r isKindOfClass:[NSDictionary class]] )
        {
            NSMutableArray* said = [NSMutableArray array];
            for( const std::string& a : g_asked )
                [said addObject:ns( a )];
            NSMutableDictionary* m = [r mutableCopy];
            m[@"kicad_said"] = said;
            r = m;
        }
        reply = @{ @"id" : cid, @"ok" : @YES, @"result" : r ?: [NSNull null] };
    }
    catch( const std::exception& e )
    {
        std::string err = e.what();
        if( !g_asked.empty() )
        {
            err += " (KiCad: ";
            for( size_t i = 0; i < g_asked.size(); ++i )
                err += ( i ? "; " : "" ) + g_asked[i];
            err += ")";
        }
        reply = @{ @"id" : cid, @"ok" : @NO, @"error" : ns( err ) };
    }

    if( --g_busy == 0 )
    {
        [g_watch invalidate];
        g_watch = nil;
        g_answer = KIPLATFORM::WORKBENCH::UNSAVED::CANCEL;
    }
    write_line( fd, reply );
    settle();
}

void listen_to( std::string path, bool owner )
{
    const int fd = socket( AF_UNIX, SOCK_STREAM, 0 );
    if( fd < 0 )
        return;
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strncpy( addr.sun_path, path.c_str(), sizeof( addr.sun_path ) - 1 );
    bool connected = false;
    for( int i = 0; i < 100 && !connected; i++ )
    {
        connected = connect( fd, reinterpret_cast<sockaddr*>( &addr ), sizeof( addr ) ) == 0;
        if( !connected )
            usleep( 50000 );
    }
    if( !connected )
    {
        close( fd );
        return;
    }
    int on = 1;
    setsockopt( fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof( on ) );
    const std::string hello = "{\"ready\":true,\"pid\":" + std::to_string( getpid() ) + "}\n";
    (void) write( fd, hello.data(), hello.size() );

    std::string buf;
    char chunk[8192];
    for( ;; )
    {
        const ssize_t n = read( fd, chunk, sizeof( chunk ) );
        if( n <= 0 )
            break;
        buf.append( chunk, size_t( n ) );
        size_t nl;
        while( ( nl = buf.find( '\n' ) ) != std::string::npos )
        {
            NSData* line = [NSData dataWithBytes:buf.data() length:nl];
            buf.erase( 0, nl + 1 );
            id msg = [NSJSONSerialization JSONObjectWithData:line options:0 error:nil];
            if( [msg isKindOfClass:[NSDictionary class]] )
            {
                dispatch_async( dispatch_get_main_queue(), ^{
                    if( msg[@"cmd"] )
                    {
                        command( fd, msg );
                    }
                    else
                    {
                        // Workbench places it as it comes forward, before macOS has finished bringing its
                        // window up: over it again once that's done
                        place( msg );
                        settle();
                    }
                } );
            }
        }
    }
    close( fd );
    // Workbench owns KiCad's lifetime while docked
    if( owner )
        dispatch_async( dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; } );
}

NSWindow* window_of( wxWindow* aFrame )
{
    if( !aFrame )
        return nil;
    NSView* view = (NSView*) aFrame->GetHandle();
    return view.window;
}


double now()
{
    return CACurrentMediaTime();
}

NSColor* hex( const std::string& h, NSColor* fallback )
{
    if( h.size() != 7 || h[0] != '#' )
        return fallback;
    const unsigned v = (unsigned) strtoul( h.c_str() + 1, nullptr, 16 );
    return [NSColor colorWithSRGBRed:( ( v >> 16 ) & 255 ) / 255.0 green:( ( v >> 8 ) & 255 ) / 255.0
                                blue:( v & 255 ) / 255.0 alpha:1];
}

NSColor* bot_color( size_t i )
{
    static const unsigned c[4] = { 0xD97757, 0xE8A55A, 0xC96442, 0xF0B07A };
    const unsigned v = c[i % 4];
    return [NSColor colorWithSRGBRed:( ( v >> 16 ) & 255 ) / 255.0 green:( ( v >> 8 ) & 255 ) / 255.0
                                blue:( v & 255 ) / 255.0 alpha:1];
}

void ensure_overlay()
{
    if( S.overlay || !g_dock )
        return;
    S.overlay = [[NSWindow alloc] initWithContentRect:g_dock.frame styleMask:NSWindowStyleMaskBorderless
                                              backing:NSBackingStoreBuffered defer:NO];
    S.overlay.opaque = NO;
    S.overlay.backgroundColor = NSColor.clearColor;
    S.overlay.ignoresMouseEvents = YES;
    S.overlay.hasShadow = NO;
    NSView* v = [[NSView alloc] initWithFrame:NSMakeRect( 0, 0, g_dock.frame.size.width, g_dock.frame.size.height )];
    v.wantsLayer = YES;
    S.overlay.contentView = v;
    // the robots' own layer, top-left origin like the rectangles KiCad hands us (AppKit manages the view's layer and
    // its flip; a sublayer's is ours)
    CALayer* root = [CALayer layer];
    root.geometryFlipped = YES;
    root.anchorPoint = CGPointZero;
    root.frame = v.layer.bounds;
    [v.layer addSublayer:root];
    S.root = root;
    [g_dock addChildWindow:S.overlay ordered:NSWindowAbove];
}

void follow_dock()
{
    if( !S.overlay || !g_dock )
        return;
    if( !NSEqualRects( S.overlay.frame, g_dock.contentLayoutRect.size.width > 0
                                            ? [g_dock convertRectToScreen:g_dock.contentLayoutRect]
                                            : g_dock.frame ) )
        [S.overlay setFrame:[g_dock convertRectToScreen:g_dock.contentLayoutRect] display:NO];
    if( S.root && !CGSizeEqualToSize( S.root.bounds.size, S.overlay.contentView.bounds.size ) )
        S.root.frame = S.overlay.contentView.bounds;
    if( S.overlay.parentWindow != g_dock )
    {
        [S.overlay.parentWindow removeChildWindow:S.overlay];
        [g_dock addChildWindow:S.overlay ordered:NSWindowAbove];
    }
}

NSPoint corner( size_t i )
{
    const NSSize sz = S.overlay ? S.overlay.frame.size : NSMakeSize( 800, 600 );
    return NSMakePoint( sz.width - 40 - ( i % 2 ) * 18, sz.height - 30 - ( i / 2 ) * 16 );
}

CALayer* make_robot( NSColor* color )
{
    CALayer* l = [CALayer layer];
    l.anchorPoint = CGPointZero;
    l.bounds = CGRectMake( 0, 0, 30, 30 );
    CAShapeLayer* arrow = [CAShapeLayer layer];
    CGMutablePathRef p = CGPathCreateMutable();
    CGPathMoveToPoint( p, nullptr, 0, 0 );
    CGPathAddLineToPoint( p, nullptr, 0, 17 );
    CGPathAddLineToPoint( p, nullptr, 4.5, 12.5 );
    CGPathAddLineToPoint( p, nullptr, 8, 19 );
    CGPathAddLineToPoint( p, nullptr, 10.5, 17.8 );
    CGPathAddLineToPoint( p, nullptr, 7.2, 11.4 );
    CGPathAddLineToPoint( p, nullptr, 13, 11.4 );
    CGPathCloseSubpath( p );
    arrow.path = p;
    CGPathRelease( p );
    arrow.fillColor = color.CGColor;
    arrow.strokeColor = NSColor.whiteColor.CGColor;
    arrow.lineWidth = 1.3;
    arrow.lineJoin = kCALineJoinRound;
    [l addSublayer:arrow];
    CALayer* head = [CALayer layer];
    head.frame = CGRectMake( 12, 14, 13, 10 );
    head.backgroundColor = color.CGColor;
    head.cornerRadius = 3;
    head.borderColor = NSColor.whiteColor.CGColor;
    head.borderWidth = 1;
    for( int i = 0; i < 2; i++ )
    {
        CALayer* eye = [CALayer layer];
        eye.frame = CGRectMake( i == 0 ? 3 : 8, 3, 2, 3 );
        eye.backgroundColor = NSColor.whiteColor.CGColor;
        [head addSublayer:eye];
    }
    [l addSublayer:head];
    return l;
}

double ease( double t )
{
    t = std::clamp( t, 0.0, 1.0 );
    return t * t * ( 3 - 2 * t );
}

NSPoint lerp( NSPoint a, NSPoint b, double k )
{
    return NSMakePoint( a.x + ( b.x - a.x ) * k, a.y + ( b.y - a.y ) * k );
}

void tick();

void ensure_timer()
{
    if( S.timer )
        return;
    S.timer = [NSTimer timerWithTimeInterval:1.0 / 60 repeats:YES block:^( NSTimer* ) { tick(); }];
    [[NSRunLoop mainRunLoop] addTimer:S.timer forMode:NSRunLoopCommonModes];
}

double rnd()
{
    S.seed = S.seed * 1664525u + 1013904223u;
    return ( ( S.seed >> 8 ) & 0xFFFF ) / 65535.0;
}

double ease_io( double t )
{
    t = std::clamp( t, 0.0, 1.0 );
    return t < 0.5 ? 4 * t * t * t : 1 - std::pow( -2 * t + 2, 3 ) / 2;
}

double ease_out( double t )
{
    t = std::clamp( t, 0.0, 1.0 );
    return 1 - std::pow( 1 - t, 3 );
}

NSColor* rgb( unsigned v, NSColor* fallback )
{
    if( !v )
        return fallback;
    return [NSColor colorWithSRGBRed:( ( v >> 16 ) & 255 ) / 255.0 green:( ( v >> 8 ) & 255 ) / 255.0
                                blue:( v & 255 ) / 255.0 alpha:1];
}

NSSize view_size()
{
    return S.overlay ? S.overlay.frame.size : NSMakeSize( 800, 600 );
}

NSPoint map( const Plan& p, const PT& q )
{
    return NSMakePoint( p.m[0] * q.x + p.m[1] * q.y + p.m[2], p.m[3] * q.x + p.m[4] * q.y + p.m[5] );
}

double scale_of( const Plan& p )
{
    return std::hypot( p.m[0], p.m[3] );
}

double dist( NSPoint a, NSPoint b )
{
    return std::hypot( b.x - a.x, b.y - a.y );
}

void add_bot( NSPoint at )
{
    Bot b;
    b.color = bot_color( S.next++ );
    b.pos = b.from = at;
    b.layer = make_robot( b.color );
    b.layer.position = b.pos;
    [S.root addSublayer:b.layer];
    S.bots.push_back( b );
}

void add_bot()
{
    add_bot( corner( S.bots.size() ) );
}

Plan* plan_of( long id )
{
    for( Plan& p : S.plans )
        if( p.id == id )
            return &p;
    return nullptr;
}

// a piece made ready to work: a trace's strokes in segments, a text's characters counted
void prepare( Piece& pc )
{
    const PIECE& s = pc.spec;
    switch( s.kind )
    {
    case PIECE::TRACE:
    {
        int    n = 0;
        PT     last{ NAN, NAN };
        bool   lifted = true;
        for( const PT& q : s.pts )
        {
            if( std::isnan( q.x ) )
            {
                lifted = true;
                continue;
            }
            if( !std::isnan( last.x ) )
            {
                Seg g{ last, q, !lifted, lifted ? -1 : n };
                if( !lifted )
                    ++n;
                pc.segs.push_back( g );
            }
            last = q;
            lifted = false;
        }
        pc.steps = (int) pc.segs.size();
        break;
    }
    case PIECE::STAMP: pc.steps = (int) s.pts.size() / 2; break;
    case PIECE::TYPE:
        pc.str = [NSString stringWithUTF8String:s.text.c_str()] ?: @"";
        pc.steps = (int) pc.str.length;
        break;
    case PIECE::BOX: pc.steps = 1; break;
    }
}

NSPoint stamp_center( const Plan& p, const Piece& pc, int i )
{
    const PT& a = pc.spec.pts[i * 2];
    const PT& b = pc.spec.pts[i * 2 + 1];
    return map( p, PT{ ( a.x + b.x ) / 2, ( a.y + b.y ) / 2 } );
}

NSPoint text_at( const Plan& p, const Piece& pc )
{
    return pc.spec.pts.empty() ? NSZeroPoint : map( p, pc.spec.pts.front() );
}

double char_w( const Plan& p, const Piece& pc )
{
    return std::clamp( pc.spec.size * scale_of( p ), 5.0, 48.0 ) * 0.62;
}

// where a piece's work starts on screen
NSPoint anchor( const Plan& p, const Piece& pc )
{
    switch( pc.spec.kind )
    {
    case PIECE::TRACE:
        return pc.segs.empty() ? ( pc.spec.pts.empty() ? NSZeroPoint : map( p, pc.spec.pts.front() ) )
                               : map( p, pc.segs.front().a );
    case PIECE::STAMP: return pc.steps ? stamp_center( p, pc, 0 ) : NSZeroPoint;
    case PIECE::TYPE: return text_at( p, pc );
    case PIECE::BOX: return pc.rect.origin;
    }
    return NSZeroPoint;
}

// how long the step in hand takes (browser seconds, before the tempo)
double step_time( const Plan& p, const Piece& pc )
{
    switch( pc.spec.kind )
    {
    case PIECE::TRACE:
    {
        const Seg& g = pc.segs[pc.at];
        const double px = dist( map( p, g.a ), map( p, g.b ) );
        return g.pen ? std::clamp( 0.08 + std::sqrt( px ) * 0.02, 0.09, 0.6 )
                     : std::clamp( 0.06 + std::sqrt( px ) * 0.015, 0.06, 0.4 );
    }
    case PIECE::STAMP:
        return pc.at == 0 ? 0.14 : std::clamp( 0.1 + std::sqrt( dist( stamp_center( p, pc, pc.at - 1 ),
                                                                       stamp_center( p, pc, pc.at ) ) ) * 0.012,
                                               0.12, 0.35 );
    case PIECE::TYPE:
    {
        const double k = std::clamp( 70.0 / std::max( 1, pc.steps ), 0.06, 1.0 );
        const unichar ch = pc.at < pc.steps ? [pc.str characterAtIndex:pc.at] : ' ';
        return ( 0.012 + rnd() * 0.028 + ( ch == ' ' ? rnd() * 0.03 : 0 ) ) * k * 1.6;
    }
    case PIECE::BOX: return 0.75;
    }
    return 0.2;
}

// where the robot is while it works: on the step in hand
NSPoint work_point( const Plan& p, Piece& pc )
{
    switch( pc.spec.kind )
    {
    case PIECE::TRACE:
    {
        if( pc.segs.empty() )
            return anchor( p, pc );
        const Seg& g = pc.segs[std::min( pc.at, (int) pc.segs.size() - 1 )];
        const double k = pc.at >= (int) pc.segs.size() ? 1.0 : ( g.pen ? pc.frac : ease_io( pc.frac ) );
        return lerp( map( p, g.a ), map( p, g.b ), k );
    }
    case PIECE::STAMP:
    {
        const int i = std::min( pc.at, pc.steps - 1 );
        if( i <= 0 )
            return stamp_center( p, pc, 0 );
        return lerp( stamp_center( p, pc, i - 1 ), stamp_center( p, pc, i ), ease_out( pc.frac * 1.6 ) );
    }
    case PIECE::TYPE:
    {
        const NSPoint a = text_at( p, pc );
        const double  h = std::clamp( pc.spec.size * scale_of( p ), 5.0, 48.0 );
        return NSMakePoint( a.x + ( pc.at + pc.frac ) * char_w( p, pc ), a.y + h );
    }
    case PIECE::BOX:
    {
        const double k = ease_io( pc.frac );
        return NSMakePoint( pc.rect.origin.x + pc.rect.size.width * k, pc.rect.origin.y + pc.rect.size.height * k );
    }
    }
    return NSZeroPoint;
}

void finish_step( Piece& pc )
{
    if( pc.spec.kind == PIECE::TRACE )
    {
        const Seg& g = pc.segs[pc.at];
        if( g.pen && g.step >= 0 && pc.spec.step )
            pc.spec.step( g.step );
    }
    else if( pc.spec.kind == PIECE::STAMP && pc.spec.step )
    {
        pc.spec.step( pc.at );
    }
    ++pc.at;
    pc.frac = 0;
}

void finish_piece( Plan& p, Piece& pc )
{
    while( pc.at < pc.steps )
        finish_step( pc );
    pc.state = 2;
    pc.done_at = now();
    if( pc.spec.done )
        pc.spec.done();
    bool all = true;
    for( const Piece& x : p.pieces )
        all = all && x.state == 2;
    if( all && !p.finished )
    {
        p.finished = true;
        p.finished_at = now();
        if( p.done )
            p.done();
    }
}

// a plan worked off at once (the swarm off, or no window to draw over)
void run_now( Plan& p )
{
    for( Piece& pc : p.pieces )
        if( pc.state != 2 )
            finish_piece( p, pc );
}

bool ready( const Plan& p, const Piece& pc )
{
    if( pc.state != 0 )
        return false;
    for( int d : pc.spec.after )
        if( d >= 0 && d < (int) p.pieces.size() && p.pieces[d].state != 2 )
            return false;
    return true;
}

// a piece's drawing: what its robot has made so far (kept on until the piece it's held for is done, then fading)
void draw_piece( Plan& p, Piece& pc, NSColor* acc, double t )
{
    double alpha = 1;
    if( pc.state == 0 )
        alpha = 0;
    else if( pc.state == 2 )
    {
        const int h = pc.spec.hold;
        const bool held = h >= 0 && h < (int) p.pieces.size() && p.pieces[h].state != 2;
        const double since = held ? 0 : t - std::max( pc.done_at, h >= 0 && h < (int) p.pieces.size() ? p.pieces[h].done_at : 0.0 );
        alpha = held ? 1 : 1 - std::clamp( since / 0.35, 0.0, 1.0 );
    }
    if( alpha <= 0 )
    {
        [pc.shape removeFromSuperlayer];
        [pc.text removeFromSuperlayer];
        pc.shape = nil;
        pc.text = nil;
        return;
    }
    NSColor* col = rgb( pc.spec.color, acc );
    const double sc = scale_of( p );
    if( pc.spec.kind == PIECE::TYPE )
    {
        if( !pc.text )
        {
            pc.text = [CATextLayer layer];
            pc.text.contentsScale = S.overlay.backingScaleFactor;
            pc.text.anchorPoint = CGPointZero;
            [S.root insertSublayer:pc.text atIndex:0];
        }
        const double h = std::clamp( pc.spec.size * sc, 5.0, 48.0 );
        const int    n = std::min( pc.at + ( pc.frac > 0.5 ? 1 : 0 ), pc.steps );
        pc.text.string = [pc.str substringToIndex:n];
        pc.text.font = (__bridge CFTypeRef) [NSFont monospacedSystemFontOfSize:h weight:NSFontWeightRegular];
        pc.text.fontSize = h;
        pc.text.foregroundColor = [col colorWithAlphaComponent:alpha].CGColor;
        const NSPoint a = text_at( p, pc );
        pc.text.frame = CGRectMake( a.x, a.y - h * 0.15, std::max( 1, pc.steps ) * char_w( p, pc ) + h, h * 1.35 );
        return;
    }
    if( !pc.shape )
    {
        pc.shape = [CAShapeLayer layer];
        pc.shape.lineCap = kCALineCapRound;
        pc.shape.lineJoin = kCALineJoinRound;
        [S.root insertSublayer:pc.shape atIndex:0];
    }
    CGMutablePathRef path = CGPathCreateMutable();
    switch( pc.spec.kind )
    {
    case PIECE::TRACE:
    {
        for( int i = 0; i < (int) pc.segs.size() && i <= pc.at; ++i )
        {
            const Seg& g = pc.segs[i];
            if( !g.pen )
                continue;
            const NSPoint a = map( p, g.a );
            const NSPoint b = i < pc.at || pc.state == 2 ? map( p, g.b ) : lerp( a, map( p, g.b ), pc.frac );
            CGPathMoveToPoint( path, nullptr, a.x, a.y );
            CGPathAddLineToPoint( path, nullptr, b.x, b.y );
        }
        pc.shape.path = path;
        pc.shape.fillColor = NSColor.clearColor.CGColor;
        pc.shape.strokeColor = [col colorWithAlphaComponent:0.95 * alpha].CGColor;
        pc.shape.lineWidth = std::clamp( pc.spec.width * sc, 1.4, 14.0 );
        break;
    }
    case PIECE::STAMP:
    {
        const int n = pc.state == 2 ? pc.steps : std::min( pc.at + 1, pc.steps );
        for( int i = 0; i < n; ++i )
        {
            NSPoint a = map( p, pc.spec.pts[i * 2] ), b = map( p, pc.spec.pts[i * 2 + 1] );
            double  k = i < pc.at || pc.state == 2 ? 1.0 : 0.55 + 0.45 * ease_out( pc.frac * 1.8 );
            const NSPoint c = lerp( a, b, 0.5 );
            const double  w = std::max( 2.0, std::abs( b.x - a.x ) * k ), h = std::max( 2.0, std::abs( b.y - a.y ) * k );
            const CGRect  r = CGRectMake( c.x - w / 2, c.y - h / 2, w, h );
            const bool    round = i < (int) pc.spec.round.size() && pc.spec.round[i];
            if( round )
                CGPathAddEllipseInRect( path, nullptr, r );
            else
            {
                const double rr = std::min( w, h ) * 0.18;
                CGPathAddRoundedRect( path, nullptr, r, rr, rr );
            }
        }
        pc.shape.path = path;
        pc.shape.fillColor = [col colorWithAlphaComponent:0.9 * alpha].CGColor;
        pc.shape.strokeColor = NSColor.clearColor.CGColor;
        break;
    }
    case PIECE::BOX:
    {
        const double k = pc.state == 2 ? 1.0 : ease_io( pc.frac );
        CGPathAddRect( path, nullptr, CGRectMake( pc.rect.origin.x, pc.rect.origin.y, pc.rect.size.width * k,
                                                   pc.rect.size.height * k ) );
        pc.shape.path = path;
        pc.shape.fillColor = [acc colorWithAlphaComponent:0.10 * alpha].CGColor;
        pc.shape.strokeColor = [acc colorWithAlphaComponent:0.9 * alpha].CGColor;
        pc.shape.lineWidth = 1.2;
        break;
    }
    default: break;
    }
    CGPathRelease( path );
}

void drop_layers( Plan& p )
{
    for( Piece& pc : p.pieces )
    {
        [pc.shape removeFromSuperlayer];
        [pc.text removeFromSuperlayer];
        pc.shape = nil;
        pc.text = nil;
    }
}

void tick()
{
    if( !S.root )
        return;
    follow_dock();
    const double t = now();
    const double dt = std::clamp( S.last > 0 ? t - S.last : 1.0 / 60, 0.0, 0.05 );
    S.last = t;
    const NSSize sz = view_size();
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    NSColor* acc = S.accent ?: [NSColor colorWithSRGBRed:0 green:0.87 blue:0.56 alpha:1];
    // each plan's view this frame (its items follow zooms, pans and resizes), each box where its item now is
    for( Plan& p : S.plans )
    {
        if( p.view )
        {
            double m[6];
            if( p.view( m ) )
                std::copy( m, m + 6, p.m );
        }
        for( Piece& pc : p.pieces )
        {
            if( pc.spec.kind != PIECE::BOX || !pc.where )
                continue;
            double x = 0, y = 0, w = 0, h = 0;
            const int r = pc.where( x, y, w, h );
            if( r == 1 )
                pc.rect = NSMakeRect( x, y, w, h );
            else if( r == 2 )
                pc.rect = NSMakeRect( x - 10, y - 10, 20, 20 );
        }
    }
    // the pieces ready to work
    std::vector<std::pair<Plan*, int>> avail;
    for( Plan& p : S.plans )
        for( int i = 0; i < (int) p.pieces.size(); ++i )
            if( ready( p, p.pieces[i] ) )
                avail.emplace_back( &p, i );
    size_t workers = 0, busy = 0;
    for( const Bot& b : S.bots )
    {
        workers += b.leaving ? 0 : 1;
        busy += b.piece >= 0 ? 1 : 0;
    }
    const double tempo = std::clamp( 2.1 + avail.size() / double( std::max<size_t>( 1, workers ) ) * 0.5, 2.1, 4.5 )
                         * ( S.on ? 1.0 : 1.6 );
    // more hands while there's more work than robots
    S.cool -= dt;
    const size_t want = std::clamp<size_t>( BASE + avail.size() / 2, BASE, MAX_BOTS );
    if( workers < want && avail.size() > workers - busy && S.cool <= 0 )
    {
        add_bot( NSMakePoint( rnd() < 0.5 ? -30 : sz.width + 30, sz.height * ( 0.2 + 0.6 * rnd() ) ) );
        S.cool = 0.06;
    }
    // a free robot takes the nearest ready piece (an earlier plan's first)
    for( Bot& b : S.bots )
    {
        if( b.piece >= 0 || b.leaving || avail.empty() )
            continue;
        size_t best = 0;
        double best_s = 1e30;
        for( size_t i = 0; i < avail.size(); ++i )
        {
            const NSPoint a = anchor( *avail[i].first, avail[i].first->pieces[avail[i].second] );
            const double  s = avail[i].first->id * 1500.0 + dist( a, b.pos );
            if( s < best_s )
            {
                best_s = s;
                best = i;
            }
        }
        Plan&  p = *avail[best].first;
        Piece& pc = p.pieces[avail[best].second];
        avail.erase( avail.begin() + best );
        pc.state = 1;
        b.plan = p.id;
        b.piece = int( &pc - &p.pieces[0] );
        b.phase = 0;
        b.t = 0;
        b.from = b.pos;
        const NSPoint a = anchor( p, pc );
        const double  d = dist( a, b.pos );
        b.dur = std::clamp( 0.12 + std::sqrt( d ) * 0.021, 0.14, 0.9 );
        b.bend = ( rnd() < 0.5 ? -1 : 1 ) * ( 0.04 + 0.1 * rnd() ) * std::min( d, 600.0 );
        b.off = a.x < 0 || a.y < 0 || a.x > sz.width || a.y > sz.height;
        b.idle = 0;
        b.step_t = b.step_dur = 0;
        if( !pc.spec.tag.empty() )
        {
            [b.tag removeFromSuperlayer];
            b.tag = [CATextLayer layer];
            b.tag.string = [NSString stringWithUTF8String:pc.spec.tag.c_str()];
            NSFont* f = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightMedium];
            b.tag.font = (__bridge CFTypeRef) f;
            b.tag.fontSize = 11;
            b.tag.foregroundColor = NSColor.whiteColor.CGColor;
            b.tag.alignmentMode = kCAAlignmentCenter;
            b.tag.contentsScale = S.overlay.backingScaleFactor;
            const NSSize ts = [(NSString*) b.tag.string sizeWithAttributes:@{ NSFontAttributeName : f }];
            b.tagSize = NSMakeSize( ts.width + 10, ts.height + 4 );
            [S.root addSublayer:b.tag];
            b.tag_at = 0;
        }
    }
    // work
    if( busy > 0 || !avail.empty() )
        S.last_work = t;
    const bool rest = t - S.last_work > 1.6;
    size_t     keep = 0;
    bool       moving = false;
    for( Bot& b : S.bots )
    {
        b.press = std::max( 0.0, b.press - dt * 6 );
        Plan* p = b.piece >= 0 ? plan_of( b.plan ) : nullptr;
        if( b.piece >= 0 && !p )
            b.piece = -1;
        if( p )
        {
            moving = true;
            Piece& pc = p->pieces[b.piece];
            double d = dt * tempo * ( b.off ? 3.0 : 1.0 );
            if( b.phase == 0 )
            {
                b.t += d;
                const NSPoint a = anchor( *p, pc );
                const double  k = std::min( 1.0, b.t / b.dur );
                const double  e = k < 0.15 ? ease_io( k ) : ease_out( k );
                const double  dx = a.x - b.from.x, dy = a.y - b.from.y, L = std::max( 1.0, std::hypot( dx, dy ) );
                const double  off = std::sin( M_PI * e ) * b.bend;
                b.pos = NSMakePoint( b.from.x + dx * e - dy / L * off, b.from.y + dy * e + dx / L * off );
                if( k >= 1 )
                {
                    b.phase = 1;
                    b.press = 1;
                    b.step_t = 0;
                    b.step_dur = 0;
                }
            }
            else if( b.phase == 1 )
            {
                // the piece's steps, one after another: each part appears as the robot finishes it
                while( d > 0 && pc.at < pc.steps )
                {
                    if( b.step_dur <= 0 )
                        b.step_dur = step_time( *p, pc );
                    const double use = std::min( d, b.step_dur - b.step_t );
                    b.step_t += use;
                    d -= use;
                    pc.frac = std::clamp( b.step_t / b.step_dur, 0.0, 1.0 );
                    if( b.step_t >= b.step_dur - 1e-9 )
                    {
                        if( pc.spec.kind == PIECE::TRACE && pc.at < (int) pc.segs.size() && pc.segs[pc.at].pen )
                            b.press = 1;
                        finish_step( pc );
                        b.step_t = 0;
                        b.step_dur = 0;
                    }
                }
                b.pos = work_point( *p, pc );
                if( pc.spec.kind != PIECE::TYPE )
                    b.press = std::max( b.press, 0.8 );
                if( pc.at >= pc.steps )
                {
                    finish_piece( *p, pc );
                    b.phase = 2;
                    b.t = 0;
                    b.dur = 0.1 + 0.08 * rnd();
                }
            }
            else
            {
                b.t += d;
                if( b.t >= b.dur )
                {
                    b.piece = -1;
                    b.plan = -1;
                    b.tag_at = t;
                }
            }
        }
        else
        {
            b.idle += dt;
            if( !b.leaving && ( ( !S.on && S.plans.empty() ) || ( b.idle > 2.5 && keep >= BASE ) ) )
                b.leaving = true;
            if( !b.leaving )
                ++keep;
            NSPoint to = b.pos;
            if( b.leaving )
                to = NSMakePoint( sz.width + 80, b.pos.y + 40 );
            else if( rest )
                to = corner( keep - 1 );
            const NSPoint n = lerp( b.pos, to, std::min( 1.0, dt * 7 ) );
            if( dist( n, b.pos ) > 0.3 )
                moving = true;
            b.pos = n;
        }
        // drawn: out of view, at the view's edge with an arrow toward the work
        NSPoint at = b.pos;
        const double pad = 28;
        if( b.piece >= 0 && ( at.x < pad || at.y < pad || at.x > sz.width - pad || at.y > sz.height - pad ) )
        {
            double dx = at.x - sz.width / 2, dy = at.y - sz.height / 2;
            const double n = std::max( 1e-3, std::hypot( dx, dy ) );
            dx /= n;
            dy /= n;
            const double k = std::min( dx != 0 ? ( sz.width / 2 - pad ) / std::abs( dx ) : 1e9,
                                       dy != 0 ? ( sz.height / 2 - pad ) / std::abs( dy ) : 1e9 );
            at = NSMakePoint( sz.width / 2 + dx * k, sz.height / 2 + dy * k );
            if( !b.arrow )
            {
                b.arrow = [CAShapeLayer layer];
                [S.root addSublayer:b.arrow];
            }
            const NSPoint c = NSMakePoint( at.x + dx * 6, at.y + dy * 6 );
            CGMutablePathRef ap = CGPathCreateMutable();
            CGPathMoveToPoint( ap, nullptr, c.x + dx * 14, c.y + dy * 14 );
            CGPathAddLineToPoint( ap, nullptr, c.x - dy * 8, c.y + dx * 8 );
            CGPathAddLineToPoint( ap, nullptr, c.x + dy * 8, c.y - dx * 8 );
            CGPathCloseSubpath( ap );
            b.arrow.path = ap;
            CGPathRelease( ap );
            b.arrow.fillColor = [acc colorWithAlphaComponent:0.95].CGColor;
        }
        else if( b.arrow )
        {
            [b.arrow removeFromSuperlayer];
            b.arrow = nil;
        }
        b.layer.position = at;
        const double s = 1 - b.press * 0.18;
        b.layer.transform = CATransform3DMakeScale( s, s, 1 );
        if( b.tag )
        {
            const double fade = b.piece >= 0 ? 1 : 1 - std::clamp( ( t - b.tag_at ) / 0.9, 0.0, 1.0 );
            if( fade <= 0 )
            {
                [b.tag removeFromSuperlayer];
                b.tag = nil;
            }
            else
            {
                const double tx = std::clamp( at.x + 28, 0.0, std::max( 0.0, sz.width - b.tagSize.width ) );
                const double ty = std::clamp( at.y + 16, 0.0, std::max( 0.0, sz.height - b.tagSize.height ) );
                b.tag.frame = CGRectMake( tx, ty, b.tagSize.width, b.tagSize.height );
                b.tag.backgroundColor = [acc colorWithAlphaComponent:0.95 * fade].CGColor;
                b.tag.opacity = fade;
            }
        }
    }
    for( auto it = S.bots.begin(); it != S.bots.end(); )
    {
        if( it->leaving && it->pos.x > sz.width + 40 )
        {
            [it->layer removeFromSuperlayer];
            [it->arrow removeFromSuperlayer];
            [it->tag removeFromSuperlayer];
            it = S.bots.erase( it );
        }
        else
            ++it;
    }
    // what's been made so far; plans done fade, then go
    for( auto it = S.plans.begin(); it != S.plans.end(); )
    {
        for( Piece& pc : it->pieces )
            draw_piece( *it, pc, acc, t );
        if( it->finished && t - it->finished_at > 0.5 )
        {
            for( Bot& b : S.bots )
                if( b.plan == it->id )
                {
                    b.plan = -1;
                    b.piece = -1;
                }
            drop_layers( *it );
            it = S.plans.erase( it );
        }
        else
            ++it;
    }
    [CATransaction commit];
    if( !S.on && S.plans.empty() && S.bots.empty() && !moving )
    {
        [S.timer invalidate];
        S.timer = nil;
    }
}
} // namespace

namespace
{
void swarm_start()
{
    ensure_overlay();
    follow_dock();
    if( !S.root )
        return; // no docked window yet: SetDockedWindow starts it
    size_t n = 0;
    for( const Bot& b : S.bots )
        n += b.leaving ? 0 : 1;
    for( ; n < BASE; ++n )
        add_bot();
    ensure_timer();
}
} // namespace

namespace KIPLATFORM
{
namespace WORKBENCH
{
bool Busy()
{
    return g_busy > 0;
}

UNSAVED UnsavedAnswer()
{
    return g_busy > 0 ? g_answer : UNSAVED::ASK;
}

void SetUnsavedAnswer( UNSAVED aAnswer )
{
    if( g_busy > 0 )
        g_answer = aAnswer;
}

void Asked( const std::string& aMessage )
{
    if( g_busy > 0 )
        g_asked.push_back( aMessage );
}

bool Docked()
{
    const char* p = getenv( "WORKBENCH_DOCK" );
    return p && p[0];
}

std::string Accent()
{
    const char* a = getenv( "WORKBENCH_ACCENT" );
    return a ? std::string( a ) : std::string();
}

void Start( std::function<std::string( const std::string& )> aHandler )
{
    static bool started = false;
    if( started || !Docked() )
        return;
    started = true;
    g_handler = std::move( aHandler );
    NSApp.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    // any window of KiCad's closing (a dialog, a progress window) can take the docked one behind Workbench's
    [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification object:nil queue:nil
                                                  usingBlock:^( NSNotification* note ) {
                                                      if( note.object != g_dock && note.object != S.overlay )
                                                          settle();
                                                  }];
    std::thread( listen_to, std::string( getenv( "WORKBENCH_DOCK" ) ), true ).detach();
    if( const char* control = getenv( "WORKBENCH_CONTROL" ); control && control[0] )
        std::thread( listen_to, std::string( control ), false ).detach();
}

void SetDockedWindow( wxWindow* aFrame )
{
    if( !Docked() )
        return;
    NSWindow* window = window_of( aFrame );
    if( !window )
        return;
    if( window == g_dock )
    {
        if( g_place )
            place( g_place ); // over Workbench's window again
        return;
    }
    if( g_dock )
    {
        if( S.overlay )
            [g_dock removeChildWindow:S.overlay];
        [g_dock orderOut:nil];
    }
    g_dock = window;
    // frameless to the eye, but still a titled window: wxWidgets closes a frame whose window turns borderless
    window.styleMask |= NSWindowStyleMaskFullSizeContentView;
    window.titlebarAppearsTransparent = YES;
    window.titleVisibility = NSWindowTitleHidden;
    for( NSWindowButton b : { NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton } )
        [window standardWindowButton:b].hidden = YES;
    window.hasShadow = NO;
    window.movable = NO;
    // with Workbench wherever it is (its own Space, full screen)
    window.collectionBehavior |= NSWindowCollectionBehaviorMoveToActiveSpace
                                 | NSWindowCollectionBehaviorFullScreenAuxiliary;
    if( S.overlay )
        [g_dock addChildWindow:S.overlay ordered:NSWindowAbove];
    if( S.on )
        swarm_start(); // the swarm turned on before there was a window to put it over
    if( g_place )
        place( g_place );
    else
        [window setFrameOrigin:NSMakePoint( -30000, -30000 )];
    settle();
}

void HideWindow( wxWindow* aFrame )
{
    if( !Docked() )
        return;
    NSWindow* window = window_of( aFrame );
    if( window && window != g_dock )
    {
        [window setFrameOrigin:NSMakePoint( -30000, -30000 )];
        [window orderOut:nil];
    }
}

void SwarmSet( bool aOn, const std::string& aAccent )
{
    S.accent = hex( aAccent.empty() ? Accent() : aAccent, S.accent );
    S.on = aOn;
    if( aOn )
        swarm_start();
    else if( S.root )
        ensure_timer();   // what's being built finishes (faster), then the robots leave
}

bool SwarmOn()
{
    return S.on;
}

void SwarmLabel( const std::string& aText )
{
    S.label = aText;
}

void SwarmPlan( std::vector<SWARM_PIECE> aPieces, SWARM_VIEW aView, std::function<void()> aDone )
{
    Plan p;
    p.id = S.next++;
    p.view = std::move( aView );
    p.done = std::move( aDone );
    for( SWARM_PIECE& s : aPieces )
    {
        Piece pc;
        pc.spec = std::move( s );
        prepare( pc );
        p.pieces.push_back( std::move( pc ) );
    }
    if( !S.label.empty() && !p.pieces.empty() && p.pieces.front().spec.tag.empty() )
        p.pieces.front().spec.tag = S.label;
    S.label.clear();
    if( !S.on || !S.root || p.pieces.empty() )
    {
        run_now( p );
        if( p.pieces.empty() && p.done )
            p.done();
        return;
    }
    if( p.view )
    {
        double m[6];
        if( p.view( m ) )
            std::copy( m, m + 6, p.m );
    }
    S.plans.push_back( std::move( p ) );
    ensure_timer();
}

void SwarmTargetLive( SWARM_WHERE aWhere, std::function<void()> aOnDrawn )
{
    double x = 0, y = 0, w = 0, h = 0;
    const int r = aWhere ? aWhere( x, y, w, h ) : 0;
    if( r == 0 || !S.on || !S.root )
    {
        if( aOnDrawn )
            aOnDrawn();
        return;
    }
    SWARM_PIECE box;
    box.kind = SWARM_PIECE::BOX;
    box.done = std::move( aOnDrawn );
    std::vector<SWARM_PIECE> pieces;
    pieces.push_back( std::move( box ) );
    const size_t before = S.plans.size();
    SwarmPlan( std::move( pieces ), nullptr, nullptr );
    if( S.plans.size() > before )
    {
        Piece& pc = S.plans.back().pieces.front();
        pc.where = std::move( aWhere );
        pc.rect = r == 1 ? NSMakeRect( x, y, w, h ) : NSMakeRect( x - 10, y - 10, 20, 20 );
    }
}

void SwarmTarget( double aX, double aY, double aW, double aH, std::function<void()> aOnDrawn )
{
    SwarmTargetLive(
            [aX, aY, aW, aH]( double& x, double& y, double& w, double& h ) -> int
            {
                x = aX;
                y = aY;
                w = aW;
                h = aH;
                return aW >= 1 && aH >= 1 ? 1 : 0;
            },
            std::move( aOnDrawn ) );
}
} // namespace WORKBENCH
} // namespace KIPLATFORM
