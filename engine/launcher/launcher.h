/*
launcher.h - AuroraOS in-process launcher (ImGui)
Copyright (C) 2026

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#pragma once
#ifndef LAUNCHER_H
#define LAUNCHER_H

#ifdef __cplusplus
extern "C" {
#endif

// Return value of Launcher_Run().
typedef enum
{
	LAUNCHER_CONTINUE = 0, // user pressed Continue → start the engine
	LAUNCHER_QUIT     = 1, // user closed the launcher → exit process
} launcher_result_t;

// Show the launcher UI. Creates the SDL window + GL context and keeps them
// alive after returning so VID_CreateWindow can reuse them. Blocks until the
// user either continues or quits.
launcher_result_t Launcher_Run( void );

// Returns the SDL_Window* the launcher created (or NULL if not launched).
// Cast to (SDL_Window *) on the caller side. Used by VID_CreateWindow to
// avoid creating a second window — AuroraOS will kill the process if its
// window disappears even briefly.
void *Launcher_GetWindow( void );

// Returns the SDL_GLContext the launcher created (or NULL). Same lifetime
// caveats as Launcher_GetWindow().
void *Launcher_GetGLContext( void );

// Called from VID_CreateWindow after the engine has taken ownership of the
// window/context. The launcher will no longer report them as its own and
// will not try to free them on engine shutdown.
void Launcher_ReleaseOwnership( void );

#ifdef __cplusplus
} // extern "C"
#endif

#endif // LAUNCHER_H
