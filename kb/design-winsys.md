# design-winsys — platform, windowing and input on KallistiOS

Replaces upstream's SDL 1.2 layer. `dc/Makefile`'s `SRC_EXCLUDE` drops
`src/winsys.c` and `src/joystick.c`; the files below supply every symbol those
two exported. **Nothing here has been executed** — both TUs compile clean and
warning-free under `kos-cc -std=gnu99 -DHAVE_CONFIG_H -Wall -Wextra -c`, and
that is the whole evidence base for runtime behaviour.

| File | Role |
|---|---|
| `dc/src/dc_winsys.c` | all of `src/winsys.h`; owns the frame loop and the maple poll |
| `dc/src/dc_joystick.c` | all of `src/joystick.h`; pure reader of the pad snapshot |
| `dc/include/dc_input.h` | shared snapshot struct + thresholds |
| `dc/include/SDL_keysym.h` | `SDLK_*` — `src/winsys.h:47` types its enum on them |
| `dc/include/SDL_mouse.h` | `SDL_BUTTON_*`, `SDL_PRESSED/RELEASED` — `winsys.h:114` |


`dc/include/SDL.h` gained two `#include` lines to pull the latter two in; real
SDL 1.2 reaches them from `SDL.h` too, so the include graph matches.
**Why SDL and not GLUT.** `src/winsys.h:25-31` `#error`s unless `HAVE_SDL` or
`HAVE_GLUT` is defined, and types `winsys_keysym_t` on whichever;
`dc/include/config.h` picks `HAVE_SDL`. A `GL/glut.h` keysym stub would leak
into nothing (`grep -rn 'GLUT_\|glut' src/*.c src/*.h` matches only
`src/winsys.[ch]`) where `HAVE_SDL` also reaches `src/gl_util.c:402`,
`src/loop.c:84` and `src/audio*.c` — but I built and then **deleted** it:
`config.h` is the shared contract and the audio shim answers those sites.

## Event model

`winsys_process_events()` is the whole loop. Per iteration: `dc_input_poll()`
→ fire pending reshape → pointer → keys → display-or-idle → `thd_pass()`. Keys
are **synthesised by diffing** a 7-bit virtual-key mask against last frame. No
repeats ever; `src/main.c:183` only asks for them off.

Enter is **13**, Escape **27** — the menus compare literally against those
(`src/race_select.c:1237,1243`, `src/event_select.c:506,512`,
`src/game_type_select.c:232,236`). `src/keyboard_util.c:61` maps the *name*
`"enter"` to `'\n'` (10); sending 10 would fire nothing. `thd_pass()` replaces
upstream's `SDL_Delay(1)` (`src/winsys.c:410`): at 60 Hz a 1 ms sleep is 6% of
the frame budget and `glKosSwapBuffers()` already blocks on vblank. **Not
measured** — revisit if audio starves.

## Input map

Bindings are upstream defaults, `src/game_config.c:537-612`, unchanged.

| DC input | Delivered as | Game action |
|---|---|---|
| Stick X | joystick x axis | steering (`racing.c:181-187`) |
| Stick Y | joystick y axis | fwd = paddle, back = brake (`racing.c:194,202`) |
| Stick | virtual cursor motion | menu pointer (frozen while racing) |
| D-pad L/R | `WSK_LEFT`/`WSK_RIGHT` | turn ("j left"/"l right"); listbox prev/next |
| D-pad Up | `WSK_UP` | paddle ("i up"); listbox prev |
| D-pad Down | `WSK_DOWN` | brake ("k space down"); listbox next |
| A | `WS_LEFT_BUTTON` + joystick button 0 | menu click; paddle; continue prompts |
| B | key 27 (Escape) | back / abort race → `GAME_OVER` (`racing.c:439-444`) |
| X | joystick button 1 | trick modifier |
| Y | joystick button 3 | jump |
| L trigger | joystick button 2 | brake |
| R trigger | folded into joystick button 0 | paddle |
| Start | key 13 (Enter) | confirm; advance splash/credits/game-over |
| Start + L trigger | key `'p'` | pause |

Button *indices* are forced by the upstream param defaults
(`game_config.c:585-606`: paddle 0, trick 1, brake 2, jump 3, continue 0), so
index 0 must be both "paddle" and "confirm". **B is not a brake**: B is
Escape, which is `quit_key` during a race (`game_config.c:537`) — doubling it
as brake would abort the run every time the player slowed, so brake is L
trigger + D-pad Down only. **Start+L for pause**: the pad has no spare button,
and sending `'p'` unconditionally would hit the practice-mode shortcut at
`src/game_type_select.c:241` while Enter is already bound there.

**Unmapped, therefore unreachable:** screenshot (`'='`), view keys
(`'1'/'2'/'3'`), reset (`backspace`), snow toggle (`tab`), and the `'c'/'w'/'m'`
race-condition toggles (`race_select.c:1249-1258` — still cursor-reachable).

## Menus: virtual cursor *and* D-pad

Both, and neither cost a `src/` change. D-pad focus nav already exists
upstream: every menu mode registers a `DEFAULT_CALLBACK` keyboard handler
switching on `WSK_UP/DOWN/LEFT/RIGHT` for listbox nav and 13/27 for
primary/back (`src/race_select.c:1212-1264`, `src/event_select.c:483-520`).

But buttons and `ssbutton`s are hit-tested against pointer coordinates
(`src/ui_mgr.c:439-511`) and several are reachable no other way. So the stick
also drives a virtual cursor fed to `winsys_mouse_func` / `winsys_motion_func`
/ `winsys_passive_motion_func`, A being the left button. Zero upstream UI
change, and the cursor draws for free: `src/ui_mgr.c:566-583` always calls
`ui_draw_cursor()` wherever we put it — which is also why
`winsys_show_cursor()` is a no-op. Coordinates are window-space, origin
top-left, the convention `src/ui_mgr.c:447,486` undoes with
`y = getparam_y_resolution() - y`. The cursor is **frozen** when all three
pointer callbacks are NULL, exactly the modes owning the stick for steering
(`src/racing.c:72-75`, `src/intro.c:68-70`); otherwise a race would park it in
a corner. `winsys_warp_pointer()` assigns position and emits **no** motion
event: `src/loop.c:145` warps to `ui_get_mouse_position()` every frame when
`capture_mouse` is on, and echoing it back would be a feedback loop.

## Screen size, GLdc init, timing

640x480, always. `winsys_init()` sets `setparam_x_resolution(640)` /
`setparam_y_resolution(480)` so every consumer sees the truth
(`src/render_util.c:45`, `src/ui_mgr.c:447`, `src/loop.c:132-146`).
`getparam_fullscreen/bpp_mode/force_window_position` are ignored; no resize
path exists. `winsys_init()` cannot fire the initial reshape — it runs at
`src/main.c:180`, before any mode registered a handler — so the loop fires
`reshape_func(640,480)` once per *newly installed* handler.

`glKosInit()` and nothing else, first thing in `winsys_init()`. **Do not call
`pvr_init()`** — GLdc does it internally, stated verbatim at
`.../kos/examples/dreamcast/gldc/basic/gl/gltest.c:37-39` and demonstrated at
`:127`, where `glKosInit()` is the first call in `main()` with no PVR setup
ahead of it. Prototypes from `$KOS_PORTS/include/GL/glkos.h`. Plain
`glKosInit()` (not `glKosInitEx()`) leaves `autosort_enabled` off, which keeps
`glDepthFunc`/`glDepthTest` working — `glkos.h:44` says autosorting *will*
break them. `initial_op/tr/pt_capacity` is the knob if lists thrash; untested.

`timer_ms_gettime64()` (`kos/timer.h:51`) serves only the cursor's frame delta,
clamped to 100 ms so a long course load cannot fling it; the game's own clock
bypasses winsys (`src/loop.c:78-92` takes the `HAVE_GETTIMEOFDAY` branch).

## Exit

`winsys_exit()` is called from inside display/idle callbacks
(`src/game_type_select.c:81,233`, `src/error_util.c:64,79`, `src/image.c`), so
`exit()` there would tear the process down with GL and game state live on the
stack. Instead it `longjmp`s to a `setjmp` at the top of
`winsys_process_events()`, which runs the atexit hook from a quiescent point,
prints `*** Tux Racer: end of run, exit code N ***`, calls `tr_harness_end()`
and **returns** — `src/main.c:275` returns 0 right after, back to the loader.

Safe only because GLdc opens and closes the PVR scene entirely inside
`glKosSwapBuffers()`, so no scene is open when a callback can call us — **read
from the GLdc API shape, not observed**; if a longjmp exit ever hangs the PVR,
check this first. Pre-loop failures (before `setjmp` ran) cannot longjmp;
there `winsys_exit()` prints "aborted before main loop" and calls `exit()`.
The exit *code* is lost either way — `main()` returns 0 unconditionally.

## Memory and known risks

- No large statics: one `dc_pad_t` (24 bytes) plus scalars. The only real cost
  is upstream's — `src/keyboard.c:29-30` sizes both key tables at `WSK_LAST` ==
  `SDLK_LAST` == 323, i.e. 2584 bytes of BSS. Shrinking `SDLK_LAST` saves ~2 KB
  and silently breaks any options file binding a function key, so it stays.
- **joyy polarity is inferred, not observed.** KOS cooks it as `raw - 128`
  (`kernel/arch/dreamcast/hardware/maple/controller.c:179`) and raw DC Y reads
  0 full-up / 255 full-down, so positive == down, matching what
  `src/racing.c:194-203` expects. If paddle and brake come out swapped on
  hardware, negate `get_joystick_y_axis()`.
- **Deadzone (24/128) and cursor speed (500 px/s) are guesses.** Untested.
