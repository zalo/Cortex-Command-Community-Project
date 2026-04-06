/**
 * EmscriptenMain.h
 *
 * Adapts the game's blocking main loops into Emscripten's per-frame callback
 * model without requiring pervasive #ifdef guards throughout Main.cpp.
 *
 * Design:
 *   The browser cannot block the main thread. Emscripten's
 *   emscripten_set_main_loop() registers a C function that gets called once
 *   per browser animation frame (requestAnimationFrame).
 *
 *   We use a simple state machine:
 *     LOADING   — data modules loading (one module per frame via yielding)
 *     MENU      — RunMenuLoop body executes one frame at a time
 *     GAME      — RunGameLoop body executes one frame at a time
 *     QUIT      — loop cancelled
 *
 *   On native builds this header is unused; Main.cpp uses its normal blocking
 *   while loops.
 *
 *   The global WebMainLoopIteration() function is referenced from
 *   WebPlatform.cpp via forward declaration.
 */

#pragma once

#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <emscripten/html5.h>

#include "System.h"
#include "TimerMan.h"
#include "WindowMan.h"
#include "FrameMan.h"
#include "RenderTarget.h"
#include "UInputMan.h"
#include "AudioMan.h"
#include "MusicMan.h"
#include "MenuMan.h"
#include "ConsoleMan.h"
#include "ActivityMan.h"
#include "MovableMan.h"
#include "SceneMan.h"
#include "LuaMan.h"
#include "PerformanceMan.h"
#include "ThreadMan.h"
#include "PostProcessMan.h"
#include "PresetMan.h"
#include "LoadingScreen.h"
#include "GameActivity.h"
#include "CameraMan.h"
#include "Box2DManager.h"
#include "FluidManager.h"
#include "Constants.h"

namespace RTE {

enum class WebLoopState {
    Menu,
    Game,
    Quit
};

static WebLoopState s_WebLoopState = WebLoopState::Menu;
static bool s_AutoStartChecked = false;
static bool s_BuildTimestampLogged = false;
static int s_LastAppliedResW = 0;
static int s_LastAppliedResH = 0;
static int s_FrameCount = 0;

/// Check if the browser container has been resized and apply the new resolution.
/// Render resolution matches the container 1:1 (container is already 16:9).
inline bool CheckAndApplyBrowserResize() {
    int browserW = EM_ASM_INT({ return window._ccPendingResizeW || 0; });
    int browserH = EM_ASM_INT({ return window._ccPendingResizeH || 0; });

    if (browserW <= 0 || browserH <= 0) return false;
    if (browserW == s_LastAppliedResW && browserH == s_LastAppliedResH) return false;

    // Skip resize for the first 120 frames (~2 seconds) to let init + autostart settle
    if (s_FrameCount < 120) return false;

    s_LastAppliedResW = browserW;
    s_LastAppliedResH = browserH;

    // Clamp to even numbers and enforce minimum 640x360
    int renderW = std::max(640, browserW & ~1);
    int renderH = std::max(360, browserH & ~1);

    if (renderW == g_WindowMan.GetResX() && renderH == g_WindowMan.GetResY()) {
        return false;
    }

    EM_ASM({ console.log('[CC] Resize: container=' + $0 + 'x' + $1); },
           renderW, renderH);

    EM_ASM({ if (window.ccResizeCanvases) ccResizeCanvases($0, $1); },
           renderW, renderH);

    g_WindowMan.ChangeResolution(renderW, renderH, 1.0f, false, false);
    return true;
}

/// Check URL ?autostart parameter and launch directly into a skirmish game.
/// Desktop: launches Skirmish Defense on Grasslands with keyboard-only P1 vs CPU.
/// Mobile: skips autostart so user lands on main menu to configure controls.
/// Usage: CortexCommand.html?autostart
inline bool CheckAutoStart() {
    if (s_AutoStartChecked) return false;
    s_AutoStartChecked = true;

    int hasParam = EM_ASM_INT({
        return (window.location.search.indexOf('autostart') >= 0) ? 1 : 0;
    });
    if (!hasParam) return false;

    EM_ASM({ console.log('[CC] AutoStart: launching Skirmish Defense on Grasslands...'); });

    // Find activity preset
    const Entity* basePreset = g_PresetMan.GetEntityPreset("GAScripted", "Skirmish Defense");
    if (!basePreset) {
        EM_ASM({ console.log('[CC] AutoStart: FAILED — could not find Skirmish Defense activity'); });
        return false;
    }
    GameActivity* game = dynamic_cast<GameActivity*>(basePreset->Clone());
    if (!game) {
        EM_ASM({ console.log('[CC] AutoStart: FAILED — clone/cast failed'); });
        return false;
    }

    // Set scene to Grasslands
    int sceneResult = g_SceneMan.SetSceneToLoad("Grasslands", true, false);
    EM_ASM({ console.log('[CC] AutoStart: SetSceneToLoad("Grasslands") result=' + $0); }, sceneResult);

    // Configure: Player 1 keyboard-only on Team 1, CPU on Team 2
    game->SetDifficulty(Activity::DifficultySetting::EasyDifficulty);
    game->SetStartingGold(5000);
    game->ClearPlayers(false);
    game->AddPlayer(Players::PlayerOne, true, Activity::Teams::TeamOne, 0);
    game->SetCPUTeam(Activity::Teams::TeamTwo);
    game->SetTeamAISkill(Activity::Teams::TeamTwo, Activity::AISkillSetting::DefaultSkill);
    game->SetTeamTech(Activity::Teams::TeamOne, "-All-");
    game->SetTeamTech(Activity::Teams::TeamTwo, "-All-");

    // Force keyboard-only for Player 1
    InputScheme* p1Scheme = g_UInputMan.GetControlScheme(Players::PlayerOne);
    if (p1Scheme) {
        p1Scheme->ResetToPlayerDefaults(Players::PlayerOne);
    }

    g_ActivityMan.SetStartActivity(game);
    g_ActivityMan.SetRestartActivity();
    EM_ASM({ console.log('[CC] AutoStart: activity configured, restart queued'); });
    return true;
}

// PollSDLEvents is defined at global scope in Main.cpp (included before this header)

/// Called once per animation frame by emscripten_set_main_loop().
/// Dispatches to the appropriate menu or game loop body.
inline void WebMainLoopIteration_Impl() {
    if (!s_BuildTimestampLogged) {
        s_BuildTimestampLogged = true;
        EM_ASM({ console.log('[CC] Build: ' + UTF8ToString($0) + ' ' + UTF8ToString($1)); },
               __DATE__, __TIME__);
    }
    s_FrameCount++;
    if (System::IsSetToQuit()) {
        s_WebLoopState = WebLoopState::Quit;
        emscripten_cancel_main_loop();
        return;
    }

    switch (s_WebLoopState) {

    // -----------------------------------------------------------------------
    case WebLoopState::Menu: {
        // AutoStart check FIRST — before resize can trigger Reinitialize()
        // which disrupts the menu state on mobile.
        bool autoStarting = false;
        if (!s_AutoStartChecked) {
            autoStarting = CheckAutoStart();
        }

        CheckAndApplyBrowserResize();
        g_WindowMan.ClearBackbuffer();
        PollSDLEvents();
        if (System::IsSetToQuit()) break;

        g_WindowMan.Update();
        g_UInputMan.Update();
        g_TimerMan.Update();
        g_TimerMan.UpdateSim();
        g_AudioMan.Update();
        g_MusicMan.Update();

        if (g_WindowMan.ResolutionChanged()) {
            // Skip menu reinitialize if we're about to autostart anyway
            if (!autoStarting) {
                g_MenuMan.Reinitialize();
            }
            g_ConsoleMan.Destroy();
            g_ConsoleMan.Initialize();
            g_LoadingScreen.CreateLoadingSplash();
            g_WindowMan.CompleteResolutionChange();
        }

        bool doneWithMenu = g_MenuMan.Update();

        if (autoStarting) {
            doneWithMenu = true;
        }

        g_ConsoleMan.Update();
        g_UInputMan.EndFrame();

        g_WindowMan.GetScreenBuffer()->Begin();
        g_MenuMan.Draw();
        g_ConsoleMan.Draw(g_FrameMan.GetBackBuffer32());
        g_WindowMan.GetScreenBuffer()->End();
        g_WindowMan.UploadFrame();

        if (doneWithMenu) {
            g_MenuMan.SetIsInMenuScreen(false);
            s_WebLoopState = WebLoopState::Game;
            g_TimerMan.ResetTime();
            g_TimerMan.PauseSim(false);
            EM_ASM({ console.log('[CC] Transitioning to Game state'); });
        }
        break;
    }

    // -----------------------------------------------------------------------
    case WebLoopState::Game: {
        CheckAndApplyBrowserResize();
        // Handle resolution change during gameplay
        if (g_WindowMan.ResolutionChanged()) {
            g_ConsoleMan.Destroy();
            g_ConsoleMan.Initialize();
            g_WindowMan.CompleteResolutionChange();
        }

        PollSDLEvents();
        if (System::IsSetToQuit()) break;

        g_WindowMan.Update();
        g_WindowMan.ClearBackbuffer();
        g_TimerMan.Update();

        // Handle pending activity restart (e.g. from autostart or scene change).
        // This must happen before the sim loop since TimeForSimUpdate() may be
        // false on the first frame after ResetTime().
        if (g_ActivityMan.ActivitySetToRestart()) {
            EM_ASM({ console.log('[CC] Game: RestartActivity (pre-sim) inActivity=' +
                     $0 + ' running=' + $1); },
                   (int)g_ActivityMan.IsInActivity(), (int)g_ActivityMan.ActivityRunning());
            g_LoadingScreen.DrawLoadingSplash();
            g_WindowMan.UploadFrame();
            bool ok = g_ActivityMan.RestartActivity();
            EM_ASM({ console.log('[CC] Game: RestartActivity returned ' + $0 +
                     ' inActivity=' + $1 + ' running=' + $2); },
                   (int)ok, (int)g_ActivityMan.IsInActivity(), (int)g_ActivityMan.ActivityRunning());
            g_TimerMan.ResetTime();
            g_TimerMan.PauseSim(false);
            g_PerformanceMan.ResetSimUpdateTimer();
            break;  // yield to browser, render next frame
        }

        // Fixed-timestep simulation (all ticks that fit in one render frame).
        // Limit to a few ticks per frame to prevent the browser from stalling
        // when the accumulator is large (e.g. after a long loading screen).
        int simTicksThisFrame = 0;
        const int maxSimTicksPerFrame = 4;
        while (g_TimerMan.TimeForSimUpdate() && simTicksThisFrame < maxSimTicksPerFrame) {
            ++simTicksThisFrame;

            g_PerformanceMan.NewPerformanceSample();
            g_PerformanceMan.UpdateMSPSU();
            g_TimerMan.UpdateSim();

            g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::SimTotal);

#define SIM_TRY(label, code) \
    try { code; } catch (const std::exception& e) { \
        EM_ASM({ console.error('[CC] CRASH in ' + UTF8ToString($0) + ': ' + UTF8ToString($1)); }, label, e.what()); \
    } catch (...) { \
        EM_ASM({ console.error('[CC] CRASH in ' + UTF8ToString($0) + ': unknown'); }, label); \
    }

            SIM_TRY("LuaMan.Update",       g_LuaMan.Update());
            SIM_TRY("UInputMan.Update",     g_UInputMan.Update());
            SIM_TRY("FrameMan.Update",      g_FrameMan.Update());
            SIM_TRY("MOID Drawings",        g_MovableMan.CompleteQueuedMOIDDrawings());
            SIM_TRY("ConsoleMan.Update",    g_ConsoleMan.Update());
            SIM_TRY("ActivityMan.Update",   g_ActivityMan.Update());

            SIM_TRY("Scene.Update",
                     if (g_SceneMan.GetScene()) g_SceneMan.GetScene()->Update());

            SIM_TRY("LuaMan.ClearTimings",  g_LuaMan.ClearScriptTimings());
            SIM_TRY("MovableMan.Update",     g_MovableMan.Update());
            SIM_TRY("ScriptTimings",         g_PerformanceMan.UpdateSortedScriptTimings(g_LuaMan.GetScriptTimings()));
            SIM_TRY("AudioMan.Update",       g_AudioMan.Update());
            SIM_TRY("MusicMan.Update",       g_MusicMan.Update());
            SIM_TRY("LateGlobalScripts",     g_ActivityMan.LateUpdateGlobalScripts());
            SIM_TRY("PresetMan.ClearReload", g_PresetMan.ClearReloadEntityPresetCalledThisUpdate());

#undef SIM_TRY

            g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::SimTotal);
            g_UInputMan.EndFrame();

            // Transition back to menu if the activity ended
            if (!g_ActivityMan.IsInActivity()) {
                g_TimerMan.PauseSim(true);
                if (!g_ActivityMan.ActivitySetToRestart()) {
                    g_MenuMan.HandleTransitionIntoMenuLoop();
                    g_MenuMan.SetIsInMenuScreen(true);
                    g_UInputMan.DisableKeys(false);
                    g_UInputMan.TrapMousePos(false);
                    s_WebLoopState = WebLoopState::Menu;
                    break;
                }
            }

            if (g_ActivityMan.ActivitySetToRestart()) {
                g_LoadingScreen.DrawLoadingSplash();
                g_WindowMan.UploadFrame();
                g_ActivityMan.RestartActivity();
                break;
            }
            if (g_ActivityMan.ActivitySetToResume()) {
                g_ActivityMan.ResumeActivity();
                g_PerformanceMan.ResetSimUpdateTimer();
            }
        }

        // Log sim tick count
        // Render frame
        g_FrameMan.Draw();

        // Toggle Box2D debug draw with F9 key
        {
            static bool f9WasDown = false;
            bool f9Down = EM_ASM_INT({ return window._ccBox2DDebug !== undefined ? window._ccBox2DDebug : 1; });
            if (!f9Down && f9WasDown) {
                // Key was released — toggle handled in JS
            }
            f9WasDown = f9Down;
            g_Box2DMan.SetDebugDraw(f9Down);
        }

        // Box2D debug overlay — draws onto the 32bpp GUI buffer
        if (g_Box2DMan.IsActive() && g_Box2DMan.IsDebugDrawEnabled()) {
            g_Box2DMan.DrawDebug();
        }

        // Fluid debug toggle (F10) and spawn (F11)
        {
            static bool f10WasDown = false;
            bool f10Down = EM_ASM_INT({ return window._ccFluidDebug ? 1 : 0; });
            if (f10Down && !f10WasDown) {
                g_FluidMan.SetDebugDraw(!g_FluidMan.IsDebugDrawEnabled());
                EM_ASM({ console.log('[Fluid] Debug draw ' + ($0 ? 'ON' : 'OFF')); },
                       g_FluidMan.IsDebugDrawEnabled());
            }
            f10WasDown = f10Down;

            // F11: spawn a 100x60 rectangle of water at screen center
            static bool f11WasDown = false;
            bool f11Down = EM_ASM_INT({ return window._ccFluidSpawn ? 1 : 0; });
            if (f11Down && !f11WasDown) {
                // Spawn at screen center using camera offset (top-left of visible area)
                Vector camOff = g_CameraMan.GetOffset(0);
                float cx = camOff.GetX() + g_FrameMan.GetPlayerScreenWidth() * 0.5f;
                float cy = camOff.GetY() + g_FrameMan.GetPlayerScreenHeight() * 0.5f;
                g_FluidMan.SpawnFluidRect(cx - 50, cy - 40, 100, 60, 160);
            }
            f11WasDown = f11Down;
        }

        // Fluid debug overlay
        if (g_FluidMan.IsEnabled() && g_FluidMan.IsDebugDrawEnabled()) {
            Vector camOffset = g_CameraMan.GetOffset(0);
            g_FluidMan.DrawDebug(g_FrameMan.GetBackBuffer32(), camOffset);
        }

        g_WindowMan.DrawPostProcessBuffer();
        g_WindowMan.UploadFrame();
        break;
    }

    case WebLoopState::Quit:
        emscripten_cancel_main_loop();
        break;
    }
}

} // namespace RTE

// WebMainLoopIteration() is defined in Main.cpp (which includes this header).
// It's forward-declared in WebPlatform.cpp — defined in Main.cpp's TU.

#endif // __EMSCRIPTEN__
