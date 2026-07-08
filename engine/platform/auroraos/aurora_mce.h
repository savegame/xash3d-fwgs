/*
 * aurora_mce.h -- portable C wrapper around the Nokia MCE (Mode Control
 * Entity) DBus service used on Aurora OS / Sailfish OS.
 *
 * Scope of this wrapper is intentionally minimal: only the display
 * blanking-pause request is exposed. The MCE service on the system bus
 * grants a ~60 second no-blank window per request, so this module also
 * carries an internal repeat timer that keeps refreshing the request
 * while blanking prevention is enabled.
 *
 * The wrapper has no dependency on the surrounding project — it only
 * needs GLib/GDBus. Drop the .h/.c pair into any project, call init()
 * once and set_prevent_blanking(true/false) around whatever "screen
 * must stay on" state you have.
 *
 * Typical use:
 *   aurora_mce_init();
 *   ...
 *   aurora_mce_set_prevent_blanking(true);   // on focus gained
 *   aurora_mce_set_prevent_blanking(false);  // on focus lost / shutdown
 *   ...
 *   aurora_mce_shutdown();
 *
 * The GLib main context (default) is iterated by aurora_mce_pump(). If
 * you already iterate the default context elsewhere, calling pump() is
 * optional.
 */

#ifndef AURORA_MCE_H
#define AURORA_MCE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Connect to the system bus and prepare the internal timer. Returns
 * false if the system bus is unreachable; in that case every other
 * entry point becomes a no-op. Safe to call multiple times.
 */
bool aurora_mce_init( void );

/* Disable blanking prevention (if active) and drop the DBus connection. */
void aurora_mce_shutdown( void );

/*
 * Iterate the default GLib main context. Optional if the host app
 * already iterates it (e.g. from another GDBus-based module).
 */
void aurora_mce_pump( void );

/*
 * Toggle screen-blanking prevention. When enabled, the wrapper fires
 * an immediate blanking-pause request and re-fires it periodically so
 * the display stays on for as long as the state remains true.
 * Idempotent: repeated calls with the same argument are no-ops.
 */
void aurora_mce_set_prevent_blanking( bool prevent );

/* Query the current state (as last requested by the caller). */
bool aurora_mce_is_preventing_blanking( void );

#ifdef __cplusplus
}
#endif

#endif /* AURORA_MCE_H */
