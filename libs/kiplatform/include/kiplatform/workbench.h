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
} // namespace WORKBENCH
} // namespace KIPLATFORM

#endif // KIPLATFORM_WORKBENCH_H_
