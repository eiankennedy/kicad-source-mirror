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
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include <wx/window.h>

namespace
{
NSWindow* g_dock = nil;     // the editor window over Workbench's panel
NSDictionary* g_place = nil; // the last placement
std::function<std::string( const std::string& )> g_handler;

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
    g_place = msg;
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
    if( [msg[@"focus"] boolValue] )
    {
        [NSApp activateIgnoringOtherApps:YES];
        [window makeKeyWindow];
    }
}

void command( int fd, NSDictionary* msg )
{
    id cid = msg[@"id"] ?: [NSNull null];
    NSData* data = [NSJSONSerialization dataWithJSONObject:msg options:0 error:nil];
    std::string request( static_cast<const char*>( data.bytes ), data.length );
    try
    {
        std::string result = g_handler ? g_handler( request ) : std::string( "null" );
        NSData* rd = [NSData dataWithBytes:result.data() length:result.size()];
        id r = [NSJSONSerialization JSONObjectWithData:rd options:NSJSONReadingFragmentsAllowed error:nil];
        write_line( fd, @{ @"id" : cid, @"ok" : @YES, @"result" : r ?: [NSNull null] } );
    }
    catch( const std::exception& e )
    {
        write_line( fd, @{ @"id" : cid, @"ok" : @NO, @"error" : [NSString stringWithUTF8String:e.what()] ?: @"failed" } );
    }
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
                        command( fd, msg );
                    else
                        place( msg );
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
    NSView* view = (__bridge NSView*) aFrame->GetHandle();
    return view.window;
}

// ---------------------------------------------------------------- the swarm

constexpr double FLY = 0.45, DRAG = 0.55, AFTER = 0.9;
constexpr size_t BASE = 4;

struct Bot
{
    CALayer* layer = nil;
    NSPoint  pos = NSZeroPoint;
    NSPoint  from = NSZeroPoint;
    double   free_at = 0;
    bool     leaving = false;
    double   press = 0;
    long     gesture = -1;
    NSColor* color = nil;
};

struct Gesture
{
    long                  id;
    size_t                bot;
    NSRect                rect; // overlay coords (top-left origin, flipped view)
    double                start;
    std::string           text;
    bool                  click;
    CAShapeLayer*         box = nil;
    CATextLayer*          tag = nil;
    std::function<void()> drawn;
    bool                  done = false;
};

struct Swarm
{
    bool                 on = false;
    NSColor*             accent = nil;
    std::string          label;
    std::vector<Bot>     bots;
    std::vector<Gesture> gestures;
    long                 next = 0;
    double               last_hit = 0;
    NSWindow*            overlay = nil;
    CALayer*             root = nil;
    NSTimer*             timer = nil;
} S;

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
    v.layer.geometryFlipped = YES; // top-left origin, like the rectangles KiCad hands us
    S.overlay.contentView = v;
    S.root = v.layer;
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

void add_bot()
{
    Bot b;
    b.color = bot_color( S.bots.size() );
    b.pos = b.from = corner( S.bots.size() );
    b.layer = make_robot( b.color );
    b.layer.position = b.pos;
    [S.root addSublayer:b.layer];
    S.bots.push_back( b );
}

void tick()
{
    if( !S.root )
        return;
    follow_dock();
    const double t = now();
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    NSColor* acc = S.accent ?: [NSColor colorWithSRGBRed:0 green:0.87 blue:0.56 alpha:1];
    bool busy = false;
    for( auto it = S.gestures.begin(); it != S.gestures.end(); )
    {
        Gesture& g = *it;
        const double k = ease( ( t - g.start - FLY ) / DRAG );
        const double fade = 1 - std::clamp( ( t - g.start - FLY - DRAG ) / AFTER, 0.0, 1.0 );
        if( !g.done && t >= g.start + FLY + DRAG )
        {
            g.done = true;
            if( g.drawn )
                g.drawn();
        }
        if( t > g.start + FLY + DRAG + AFTER )
        {
            [g.box removeFromSuperlayer];
            [g.tag removeFromSuperlayer];
            it = S.gestures.erase( it );
            continue;
        }
        busy = true;
        if( t >= g.start + FLY )
        {
            if( !g.box )
            {
                g.box = [CAShapeLayer layer];
                g.box.lineWidth = 1.2;
                [S.root insertSublayer:g.box atIndex:0];
            }
            if( g.click )
            {
                const double r = 6 + 16 * k;
                const NSPoint c = NSMakePoint( NSMidX( g.rect ), NSMidY( g.rect ) );
                CGPathRef p = CGPathCreateWithEllipseInRect( CGRectMake( c.x - r, c.y - r, 2 * r, 2 * r ), nullptr );
                g.box.path = p;
                CGPathRelease( p );
                g.box.fillColor = NSColor.clearColor.CGColor;
                g.box.strokeColor = [acc colorWithAlphaComponent:0.8 * ( 1 - k )].CGColor;
            }
            else
            {
                CGPathRef p = CGPathCreateWithRect(
                        CGRectMake( g.rect.origin.x, g.rect.origin.y, g.rect.size.width * k, g.rect.size.height * k ),
                        nullptr );
                g.box.path = p;
                CGPathRelease( p );
                g.box.fillColor = [acc colorWithAlphaComponent:0.10 * fade].CGColor;
                g.box.strokeColor = [acc colorWithAlphaComponent:0.9 * fade].CGColor;
            }
            if( !g.text.empty() )
            {
                if( !g.tag )
                {
                    g.tag = [CATextLayer layer];
                    g.tag.string = [NSString stringWithUTF8String:g.text.c_str()];
                    g.tag.fontSize = 11;
                    g.tag.font = (__bridge CFTypeRef)[NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightMedium];
                    g.tag.foregroundColor = NSColor.whiteColor.CGColor;
                    g.tag.alignmentMode = kCAAlignmentCenter;
                    g.tag.contentsScale = S.overlay.backingScaleFactor;
                    const NSSize ts = [g.tag.string sizeWithAttributes:@{
                        NSFontAttributeName : [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightMedium]
                    }];
                    g.tag.frame = CGRectMake( g.rect.origin.x, g.rect.origin.y - ts.height - 6, ts.width + 10,
                                              ts.height + 4 );
                    [S.root addSublayer:g.tag];
                }
                g.tag.backgroundColor = [acc colorWithAlphaComponent:0.95 * fade].CGColor;
                g.tag.opacity = fade;
            }
        }
        ++it;
    }
    const bool idle = t - S.last_hit > 1.6;
    bool moving = false;
    for( size_t i = 0; i < S.bots.size(); ++i )
    {
        Bot& b = S.bots[i];
        const Gesture* g = nullptr;
        for( const Gesture& x : S.gestures )
            if( x.bot == i && t >= x.start && t < x.start + FLY + DRAG + 0.05 )
            {
                g = &x;
                break;
            }
        if( g )
        {
            const NSPoint start = g->click ? NSMakePoint( NSMidX( g->rect ), NSMidY( g->rect ) ) : g->rect.origin;
            if( b.gesture != g->id )
            {
                b.gesture = g->id;
                b.from = b.pos;
            }
            if( t < g->start + FLY )
            {
                b.pos = lerp( b.from, start, ease( ( t - g->start ) / FLY ) );
                b.press = 0;
            }
            else
            {
                const NSPoint end = g->click ? start : NSMakePoint( NSMaxX( g->rect ), NSMaxY( g->rect ) );
                b.pos = lerp( start, end, ease( ( t - g->start - FLY ) / DRAG ) );
                b.press = 1;
            }
            moving = true;
        }
        else
        {
            b.gesture = -1;
            b.press = std::max( 0.0, b.press - 0.1 );
            NSPoint to = b.pos;
            if( !S.on || ( idle && i >= BASE ) )
            {
                b.leaving = true;
                to = NSMakePoint( S.overlay.frame.size.width + 60, S.overlay.frame.size.height + 60 );
            }
            else if( idle )
            {
                to = corner( i );
            }
            const NSPoint n = lerp( b.pos, to, 0.12 );
            if( std::hypot( n.x - b.pos.x, n.y - b.pos.y ) > 0.3 )
                moving = true;
            b.pos = n;
        }
        b.layer.position = b.pos;
        const double s = 1 - b.press * 0.18;
        b.layer.transform = CATransform3DMakeScale( s, s, 1 );
    }
    const NSSize sz = S.overlay.frame.size;
    for( auto it = S.bots.begin(); it != S.bots.end(); )
    {
        if( it->leaving && ( it->pos.x > sz.width + 40 || it->pos.y > sz.height + 40 ) )
        {
            [it->layer removeFromSuperlayer];
            it = S.bots.erase( it );
        }
        else
        {
            ++it;
        }
    }
    [CATransaction commit];
    if( !S.on && !busy && !moving && S.bots.empty() )
    {
        [S.timer invalidate];
        S.timer = nil;
    }
}
} // namespace

namespace KIPLATFORM
{
namespace WORKBENCH
{
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
    std::thread( listen_to, std::string( getenv( "WORKBENCH_DOCK" ) ), true ).detach();
    if( const char* control = getenv( "WORKBENCH_CONTROL" ); control && control[0] )
        std::thread( listen_to, std::string( control ), false ).detach();
}

void SetDockedWindow( wxWindow* aFrame )
{
    if( !Docked() )
        return;
    NSWindow* window = window_of( aFrame );
    if( !window || window == g_dock )
        return;
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
    if( S.overlay )
        [g_dock addChildWindow:S.overlay ordered:NSWindowAbove];
    if( g_place )
        place( g_place );
    else
        [window setFrameOrigin:NSMakePoint( -30000, -30000 )];
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
    {
        ensure_overlay();
        follow_dock();
        if( !S.root )
            return;
        while( S.bots.size() < BASE )
            add_bot();
        ensure_timer();
    }
    else
    {
        for( Gesture& g : S.gestures )
        {
            if( !g.done && g.drawn )
                g.drawn();
            [g.box removeFromSuperlayer];
            [g.tag removeFromSuperlayer];
        }
        S.gestures.clear();
    }
}

bool SwarmOn()
{
    return S.on;
}

void SwarmLabel( const std::string& aText )
{
    S.label = aText;
}

void SwarmTarget( double aX, double aY, double aW, double aH, std::function<void()> aOnDrawn )
{
    if( !S.on || !S.root || aW < 3 || aH < 3 )
    {
        if( aOnDrawn )
            aOnDrawn();
        return;
    }
    const double t = now();
    const NSPoint c = NSMakePoint( aX + aW / 2, aY + aH / 2 );
    size_t best = SIZE_MAX;
    double dist = 1e18;
    for( size_t i = 0; i < S.bots.size(); ++i )
    {
        const Bot& b = S.bots[i];
        if( b.leaving || b.free_at > t )
            continue;
        const double d = std::hypot( b.pos.x - c.x, b.pos.y - c.y );
        if( d < dist )
        {
            dist = d;
            best = i;
        }
    }
    if( best == SIZE_MAX ) // every robot busy: another joins, as many as the work needs
    {
        add_bot();
        best = S.bots.size() - 1;
    }
    Bot& bot = S.bots[best];
    const double start = std::max( t, bot.free_at );
    bot.free_at = start + FLY + DRAG + 0.05;
    bot.leaving = false;
    Gesture g;
    g.id = S.next++;
    g.bot = best;
    g.rect = NSMakeRect( aX, aY, aW, aH );
    g.start = start;
    g.text = S.label;
    S.label.clear();
    g.click = aW < 40 && aH < 30;
    g.drawn = std::move( aOnDrawn );
    S.gestures.push_back( std::move( g ) );
    S.last_hit = std::max( S.last_hit, start + FLY + DRAG );
    ensure_timer();
}
} // namespace WORKBENCH
} // namespace KIPLATFORM
