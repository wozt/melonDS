#ifndef BOTTOMSCREENBRIDGE_H
#define BOTTOMSCREENBRIDGE_H

#include <cstdint>

/*
 * The melonDS side of bottom_screen_server.
 *
 * Everything below this header is plain C in the bottom_screen_server
 * tree, shared with the standalone binary and, later, with the Azahar
 * and Cemu backends. This file is the only C++ in the path, and it is
 * only here because melonDS's API is C++.
 *
 * It is driven entirely from EmuThread, which is already a friend of
 * EmuInstance and already reads the framebuffer and writes the input
 * fields once per frame. Hooking there means EmuInstance itself needs no
 * change at all, and the emulator does exactly one new thing per frame:
 * hand over a pointer.
 *
 * Controlled by environment variables rather than a settings dialog:
 *
 *   BOTTOM_SCREEN=0      turn it off entirely
 *   BOTTOM_SCREEN_PORT   listen port
 *
 * Both override the saved settings, which is what a scripted launch
 * wants; without a variable the settings decide.
 */

namespace BottomScreen
{

/*
 * Idempotent. Called from the emu thread the first time a frame is
 * submitted, so nothing happens until a game is actually running.
 *
 * Reads melonDS's own settings, BottomScreen.Enabled and
 * BottomScreen.Port. The environment variables still win when set,
 * which keeps a scripted launch able to override a saved setting
 * without editing anyone's config file.
 */
void Start(bool enabled, int port);
void Stop();
bool IsRunning();

/*
 * 256x192 BGRA, tightly packed, exactly what
 * GPU::GetFramebuffers gives for the bottom screen with the software
 * renderer. The pixels are copied, so the caller's buffer is free the
 * moment this returns.
 */
void SubmitFrame(const void* bottomBGRA);

/*
 * Buttons held by connected clients, as a mask in melonDS's own bit
 * order -- bit set means held. melonDS's inputMask is active low, so
 * the caller ands with the complement.
 */
uint32_t PressedKeys();

/* Returns true and fills x/y while a client is touching the screen. */
bool TouchState(uint16_t& x, uint16_t& y);

/*
 * Called when GetFramebuffers reports that the bottom screen is not in
 * RAM -- the OpenGL renderer, where the pointer is a texture handle.
 * Warns once instead of silently streaming nothing.
 */
void ReportGpuRenderer();

}

#endif // BOTTOMSCREENBRIDGE_H
