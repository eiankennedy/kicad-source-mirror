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

// Workbench docking is macOS only (os/apple/workbench.mm); elsewhere nothing happens.

#include <kiplatform/workbench.h>

namespace KIPLATFORM
{
namespace WORKBENCH
{
bool Docked() { return false; }
std::string Accent() { return std::string(); }
bool Busy() { return false; }
UNSAVED UnsavedAnswer() { return UNSAVED::ASK; }
void SetUnsavedAnswer( UNSAVED ) {}
void Asked( const std::string& ) {}
void Start( std::function<std::string( const std::string& )> ) {}
void SetDockedWindow( wxWindow* ) {}
void HideWindow( wxWindow* ) {}
void SwarmSet( bool, const std::string& ) {}
bool SwarmOn() { return false; }
void SwarmLabel( const std::string& ) {}
void SwarmTarget( double, double, double, double, std::function<void()> aOnDrawn )
{
    if( aOnDrawn )
        aOnDrawn();
}
} // namespace WORKBENCH
} // namespace KIPLATFORM
