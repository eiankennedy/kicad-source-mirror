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

#ifndef KIPLATFORM_WORKBENCH_H_
#define KIPLATFORM_WORKBENCH_H_

#include <functional>
#include <string>
#include <vector>

class wxWindow;

/**
 * Docking into Workbench (a Mac app that runs KiCad in its Electrical panel). When WORKBENCH_DOCK names a Unix
 * socket (the Mac app's), the editor Workbench shows is drawn without a frame and placed over its panel; the
 * other KiCad windows stay out of sight. WORKBENCH_CONTROL names a second socket (Workbench's server's) for its
 * commands. One JSON object per line each way:
 *   placement: {"rect": [x, y, w, h] (screen points, bottom-left origin), "above": <window number>, "visible": bool}
 *   command:   {"id": n, "cmd": "...", ...}  ->  {"id": n, "ok": true, "result": ...} | {"id": n, "ok": false, ...}
 * Elsewhere (not macOS, or not docked) everything here does nothing.
 */
namespace KIPLATFORM
{
namespace WORKBENCH
{
    /// True when KiCad runs docked in Workbench.
    bool Docked();

    /// The session's accent from Workbench ("#rrggbb"), or empty.
    std::string Accent();

    /// Start listening to Workbench. The handler runs on the main thread with one command (a JSON object, as text)
    /// and returns the result as JSON text; it throws std::exception for an error.
    void Start( std::function<std::string( const std::string& )> aHandler );

    /// What a "Save changes?" question gets while one of Workbench's commands runs: no one could answer a dialog
    /// then (KiCad is out of sight or driven by Claude), so none is shown.
    enum class UNSAVED
    {
        ASK,       ///< no command running: ask as usual
        SAVE,      ///< save first (opening another project)
        DISCARD,   ///< the file on disk wins (reloading after Claude wrote it)
        CANCEL     ///< anything else: the command stops with an error rather than wait on a dialog
    };

    /// True while one of Workbench's commands runs (on the main thread).
    bool Busy();

    /// The answer to a "Save changes?" question now (ASK when no command runs).
    UNSAVED UnsavedAnswer();

    /// For the command running now: how a "Save changes?" question is answered (CANCEL by default).
    void SetUnsavedAnswer( UNSAVED aAnswer );

    /// A question or message KiCad would have shown while a command runs (no dialog is shown): the command's
    /// error says it when the command fails.
    void Asked( const std::string& aMessage );

    /// The window Workbench places over its panel (an editor frame); any earlier one goes out of sight.
    void SetDockedWindow( wxWindow* aFrame );

    /// Keep a window of KiCad's out of sight while docked (the project manager, an editor not shown).
    void HideWindow( wxWindow* aFrame );

    /// Workbench's build swarm over the docked window: robot cursors that act out each change.
    void SwarmSet( bool aOn, const std::string& aAccent );
    bool SwarmOn();
    /// The command the next robot's tag shows.
    void SwarmLabel( const std::string& aText );
    /// A change at this rectangle (points, from the docked window's content top-left): a robot draws it out.
    /// aOnDrawn runs on the main thread once the robot has drawn it (a new item shown then), or right away when
    /// the swarm is off.
    void SwarmTarget( double aX, double aY, double aW, double aH, std::function<void()> aOnDrawn = nullptr );
    /// Where a change is on screen now, asked every frame while its robot works (so it follows the item through
    /// zooms, pans and resizes): 1 in view (x, y, w, h as for SwarmTarget), 2 out of view (x, y: the point at the
    /// view's edge toward it; its robot works there, with an arrow), 0 gone (the last place stays).
    using SWARM_WHERE = std::function<int( double& aX, double& aY, double& aW, double& aH )>;
    void SwarmTargetLive( SWARM_WHERE aWhere, std::function<void()> aOnDrawn = nullptr );

    /// A point in the editor's own world (its internal units).
    struct SWARM_PT
    {
        double x = 0, y = 0;
    };

    /// One piece of a plan: a robot's job, the way the browser swarm rebuilds a page piece by piece.
    struct SWARM_PIECE
    {
        enum KIND
        {
            TRACE,   ///< a pen along pts (a NaN point lifts the pen between strokes): an outline, a pin, a wire, a track
            STAMP,   ///< shapes put down one after another, two corners each in pts: pads, vias, junctions
            TYPE,    ///< text typed in from pts[0] (its top-left): a reference, a value, a label
            BOX      ///< a box dragged over an item (a change)
        };
        KIND                  kind = TRACE;
        std::vector<SWARM_PT> pts;
        std::vector<char>     round;       ///< STAMP: per shape, a circle (else a rounded rectangle)
        double                width = 0;   ///< TRACE: the stroke's width in world units (0: a fine line)
        std::string           text;        ///< TYPE
        double                size = 0;    ///< TYPE: the text's height in world units
        unsigned              color = 0;   ///< 0xRRGGBB; 0: the session's accent
        std::vector<int>      after;       ///< pieces of the same plan (indices) done before this one starts
        int                   hold = -1;   ///< drawn on until that piece is done (-1: fades once this one is)
        std::string           tag;         ///< the robot's tag while it works
        std::function<void( int )> step;   ///< each pen segment / shape as its robot finishes it (main thread)
        std::function<void()> done;        ///< the piece finished (main thread)
    };

    /// The editor's world to the overlay this frame (points, from the docked window's content top-left):
    /// x' = m[0] x + m[1] y + m[2], y' = m[3] x + m[4] y + m[5]. False: no view this frame (the last one stays).
    using SWARM_VIEW = std::function<bool( double* m )>;

    /// A change as a plan of pieces for the swarm; aDone runs once every piece is done. With the swarm off, every
    /// callback runs right away.
    void SwarmPlan( std::vector<SWARM_PIECE> aPieces, SWARM_VIEW aView, std::function<void()> aDone = nullptr );
} // namespace WORKBENCH
} // namespace KIPLATFORM

#endif // KIPLATFORM_WORKBENCH_H_
