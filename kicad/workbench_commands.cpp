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

// Workbench's commands to a docked KiCad (kiplatform/workbench.h), run by the project manager:
//   state                          the project, which editor is docked
//   open {project}                 a project (.kicad_pro)
//   show {editor: "sch"|"pcb"}     that editor docked over Workbench's panel (its document loaded)
//   reload / save / hands {off} / fit / swarm {on, accent} / label {text} / inspect
//                                  handed to the docked editor (KiwayMailIn MAIL_WORKBENCH; see
//                                  eeschema/workbench_dock.cpp and pcbnew/workbench_dock.cpp)

#include "kicad_manager_frame.h"

#include <frame_type.h>
#include <kiplatform/workbench.h>
#include <kiway.h>
#include <kiway_player.h>
#include <mail_type.h>
#include <nlohmann/json.hpp>

#include <wx/filename.h>

#include <stdexcept>
#include <vector>

namespace
{
using json = nlohmann::json;

FRAME_T     g_shown = FRAME_T_COUNT;   // the editor docked now
std::string g_editor;

KIWAY_PLAYER* show_editor( KICAD_MANAGER_FRAME* aFrame, FRAME_T aType )
{
    if( !aFrame->IsProjectActive() )
        throw std::runtime_error( "open a project first" );

    KIWAY_PLAYER* player = aFrame->Kiway().Player( aType, true );

    if( !player )
        throw std::runtime_error( "the editor didn't start" );

    if( !player->IsVisible() )   // a hidden frame might not have its document loaded
    {
        wxString path;

        if( aType == FRAME_SCH )
            path = aFrame->SchFileName();
        else
            path = aFrame->PcbFileName();

        if( wxFileName( path ).FileExists() )
        {
            std::vector<wxString> files{ path };

            if( !player->OpenProjectFiles( files ) )
                throw std::runtime_error( "couldn't open " + std::string( path.ToUTF8() ) );
        }

        player->Show( true );
    }

    // the other editor goes out of sight; this one over Workbench's panel
    FRAME_T other = aType == FRAME_SCH ? FRAME_PCB_EDITOR : FRAME_SCH;

    if( KIWAY_PLAYER* o = aFrame->Kiway().Player( other, false ) )
        KIPLATFORM::WORKBENCH::HideWindow( o );

    KIPLATFORM::WORKBENCH::SetDockedWindow( player );
    KIPLATFORM::WORKBENCH::HideWindow( aFrame );   // the project manager stays out of sight
    g_shown = aType;
    return player;
}

std::string mail_docked( KICAD_MANAGER_FRAME* aFrame, const json& aRequest )
{
    if( g_shown == FRAME_T_COUNT )
        throw std::runtime_error( "no editor shown yet (show {editor})" );

    std::string payload = aRequest.dump();
    aFrame->Kiway().ExpressMail( g_shown, MAIL_WORKBENCH, payload );

    // the editor writes its answer back into the payload: {"ok": ..., "result" | "error"}
    json reply = json::parse( payload, nullptr, false );

    if( reply.is_discarded() || !reply.contains( "ok" ) )
        throw std::runtime_error( "the editor didn't answer" );

    if( !reply.value( "ok", false ) )
        throw std::runtime_error( reply.value( "error", std::string( "failed" ) ) );

    return reply.contains( "result" ) ? reply["result"].dump() : std::string( "null" );
}

// each open editor's unsaved changes saved, before its project closes
void save_editors( KICAD_MANAGER_FRAME* aFrame )
{
    for( FRAME_T type : { FRAME_SCH, FRAME_PCB_EDITOR } )
    {
        KIWAY_PLAYER* player = aFrame->Kiway().Player( type, false );

        if( !player || !player->IsContentModified() )
            continue;

        std::string payload = json( { { "cmd", "save" } } ).dump();
        aFrame->Kiway().ExpressMail( type, MAIL_WORKBENCH, payload );
        json reply = json::parse( payload, nullptr, false );

        if( reply.is_discarded() || !reply.value( "ok", false ) )
        {
            std::string why = reply.is_object() ? reply.value( "error", std::string() ) : std::string();
            throw std::runtime_error( std::string( "couldn't save the open " )
                                      + ( type == FRAME_SCH ? "schematic" : "board" ) + " first"
                                      + ( why.empty() ? "" : ": " + why ) );
        }
    }
}
} // namespace


std::string WorkbenchCommand( KICAD_MANAGER_FRAME* aFrame, const std::string& aRequest )
{
    const json        req = json::parse( aRequest );
    const std::string cmd = req.value( "cmd", "" );

    if( cmd == "state" )
    {
        json out = { { "project", aFrame->IsProjectActive()
                                          ? std::string( aFrame->GetProjectFileName().ToUTF8() )
                                          : std::string() },
                     { "editor", g_editor },
                     { "swarm", KIPLATFORM::WORKBENCH::SwarmOn() } };
        return out.dump();
    }

    if( cmd == "open" )
    {
        wxFileName fn( wxString::FromUTF8( req.value( "project", std::string() ) ) );

        if( fn.GetExt() != wxT( "kicad_pro" ) )
            fn.SetExt( wxT( "kicad_pro" ) );

        if( !fn.IsAbsolute() )
            fn.MakeAbsolute();

        if( !fn.FileExists() )
            throw std::runtime_error( "no project at " + std::string( fn.GetFullPath().ToUTF8() ) );

        // the editors' unsaved changes are saved first, silently, as Workbench's other apps do (closing the
        // project would otherwise ask, with no one to answer)
        KIPLATFORM::WORKBENCH::SetUnsavedAnswer( KIPLATFORM::WORKBENCH::UNSAVED::SAVE );
        save_editors( aFrame );

        if( !aFrame->LoadProject( fn ) )
            throw std::runtime_error( "couldn't open the project" );

        g_shown = FRAME_T_COUNT;
        g_editor.clear();
        KIPLATFORM::WORKBENCH::HideWindow( aFrame );   // loading a project brings the manager forward
        return json( { { "project", std::string( fn.GetFullPath().ToUTF8() ) } } ).dump();
    }

    if( cmd == "show" )
    {
        KIPLATFORM::WORKBENCH::SetUnsavedAnswer( KIPLATFORM::WORKBENCH::UNSAVED::SAVE );
        const std::string editor = req.value( "editor", std::string( "sch" ) );
        show_editor( aFrame, editor == "pcb" ? FRAME_PCB_EDITOR : FRAME_SCH );
        g_editor = editor;

        // the editor follows the panel's hands switch and the swarm
        json hands = { { "cmd", "hands" }, { "off", req.value( "hands_off", true ) } };
        mail_docked( aFrame, hands );
        return json( { { "editor", editor } } ).dump();
    }

    if( cmd == "swarm" )
    {
        KIPLATFORM::WORKBENCH::SwarmSet( req.value( "on", true ), req.value( "accent", std::string() ) );
        return json( { { "swarm", KIPLATFORM::WORKBENCH::SwarmOn() } } ).dump();
    }

    if( cmd == "label" )
    {
        KIPLATFORM::WORKBENCH::SwarmLabel( req.value( "text", std::string() ) );
        return "true";
    }

    if( cmd == "reload" )   // the files on disk are the truth: what the editor had unsaved goes, without asking
        KIPLATFORM::WORKBENCH::SetUnsavedAnswer( KIPLATFORM::WORKBENCH::UNSAVED::DISCARD );
    else if( cmd == "save" )
        KIPLATFORM::WORKBENCH::SetUnsavedAnswer( KIPLATFORM::WORKBENCH::UNSAVED::SAVE );

    if( cmd == "hands" || cmd == "reload" || cmd == "save" || cmd == "fit" || cmd == "inspect" )
        return mail_docked( aFrame, req );

    throw std::runtime_error( "unknown command: " + cmd );
}
