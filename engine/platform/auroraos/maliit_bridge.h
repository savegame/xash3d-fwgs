/*
 * maliit_bridge.h -- glue between the maliit_client wrapper and the
 * xash3d SDL2 platform layer.
 *
 * The wrapper is transport-agnostic; this bridge turns its callbacks into
 * SDL_PushEvent so the rest of the engine — VGui, console, menus — sees
 * regular SDL_TEXTINPUT / SDL_KEYDOWN events without knowing maliit exists.
 *
 * All entry points are safe to call before/after init; they no-op when the
 * bridge isn't ready.
 */

#ifndef MALIIT_BRIDGE_H
#define MALIIT_BRIDGE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lazy: first call initialises the wrapper. Safe to call repeatedly. */
void maliit_bridge_init( void );

/* Call from the engine shutdown path. */
void maliit_bridge_shutdown( void );

/* Iterate the GLib main context — call once per engine frame. */
void maliit_bridge_pump( void );

/*
 * Route Platform_EnableTextInput here. On enable: focus_in + show.
 * On disable: focus_out + hide.
 */
void maliit_bridge_enable_text_input( bool enable );

/* Password field hint. Applied on next enable_text_input(true). */
void maliit_bridge_set_password_mode( bool password );

#ifdef __cplusplus
}
#endif

#endif /* MALIIT_BRIDGE_H */
