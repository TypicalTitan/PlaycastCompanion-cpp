# Porting contract (read before writing any code)

This is a faithful C++20 port of the C# app in `C:\Users\turtl\repos\PlaycastCompanion`
(the **specification** — read the matching `.cs` file for the module you own and
reproduce its behaviour, log messages, status strings, and config semantics).

## Ground rules
1. **Headers in `src/` are the contract.** Public signatures are fixed. You may add
   private members/helpers to the header of a class you own (below the
   `// module owner may extend` marker), never change public ones. Never edit a
   header you don't own.
2. **Toolchain:** MSVC 14.44, C++20, `/W4 /WX /permissive- /sdl`. Your file must
   compile **warning-free**. Check it with `build.cmd check YourFile.cpp` (compiles
   one translation unit, no link). Run it before you finish.
3. **No third-party code.** Win32, WinHTTP, Winsock, GDI+, C++/WinRT (SDK headers
   only), and the C++ standard library. JSON goes through `pc::json` (Json.h),
   which wraps `winrt::Windows::Data::Json`. HTTP goes through `pc::http` (Http.h).
4. **Every .cpp starts with `#include "pch.h"`** then its own header.
5. **Unicode everywhere.** `std::wstring` for Windows/UI/config strings,
   `std::string` only for UTF-8 wire bytes. Use `pc::json::Utf8ToWide/WideToUtf8`.
6. **RAII, no leaks.** `pc::UniqueHandle` for kernel handles, `std::unique_ptr`,
   `winrt::com_ptr`. No raw `new` without an owner. No `_s`-less C string APIs.
7. **Hardening is a feature.** Validate every byte read from a pipe/socket (frame
   lengths capped at 1 MB, names capped, control chars stripped via
   `AppConfig::Sanitize`). Never trust a peer. Never show a MessageBox from a
   worker thread. Never run elevated.
8. **Threads:** use `std::jthread` + `std::stop_token`. Any thread touching WinRT
   calls `winrt::init_apartment(winrt::apartment_type::multi_threaded)` first.
   Blocking Win32 I/O must be unblockable from `Stop()` (CancelSynchronousIo,
   closing the handle, or an event wait) so stops finish within a few seconds.
9. **Logging:** `pc::LogInfo(L"...")`. Mirror the C# messages (e.g.
   `"Chroma session opened: ..."`, `"guest session active (reason) -> stealth on"`).
10. **Exceptions:** throw `std::runtime_error` with a readable message for
    failures inside `ApplyTick`; the base class handles logging/retry. Never let an
    exception escape a thread or a window procedure (catch, log, continue).
11. **Config compatibility:** the JSON key names and semantics of
    `PlaycastCompanion\config.json` and `AppConfig.cs` are law.
12. Keep each module self-contained; do not add globals other than what your
    header declares.

## Module ownership (one agent each)
| Module | Files | C# reference |
|---|---|---|
| core | Log.cpp, Json.cpp, Config.cpp, Http.cpp | Log.cs, AppConfig.cs |
| lighting-base | LightingBackend.cpp, NativeResolver.cpp | LightingBackend.cs, NativeResolver.cs |
| chroma+steelseries | ChromaController.cpp, SteelSeriesController.cpp | ChromaController.cs, SteelSeriesController.cs |
| openrgb+wdl | OpenRgbController.cpp, DynamicLightingController.cpp | OpenRgbController.cs, DynamicLightingController.cs, LampDiagnostics.cs |
| logitech+corsair | LogitechController.cpp, CorsairController.cpp | LogitechController.cs, CorsairController.cs |
| watchers | SessionWatcher.cpp, RegistryWatcher.cpp | SessionWatcher.cs, RegistryWatcher.cs |
| discord | DiscordPresence.cpp, GameNameCache.cpp | DiscordPresence.cs, GameNameCache.cs |
| shim | DiscordShim.cpp | DiscordShimServer.cs, DiscordChannelListener.cs |
| app | TrayApp.cpp, main.cpp | TrayAppContext.cs, Program.cs |
| ui | MainWindow.cpp, Branding.cpp | MainForm.cs, Branding.cs |

## main.cpp flags (owned by `app`)
`--autostart` quiet start (no window); `--snapshot <dir>` render tabs to PNG and
exit; `--lamps [file]` LampArray diagnostic and exit. Single instance per session
via mutex `Local\PlaycastCompanion`; a second launch opens the existing instance's
window by setting event `Local\PlaycastCompanion.ShowSettings` (after waiting up to
3 s for a restarting instance to release the mutex). Log and swallow unhandled
exceptions (SetUnhandledExceptionFilter) — never a crash dialog.

## Running build.cmd from a Bash/MSYS shell
MSYS rewrites `/c` into the drive path `C:\`, which makes `cmd.exe /c build.cmd ...`
launch an interactive cmd that hangs on stdin. Use either of these instead:
- PowerShell: `cmd /c "C:\Users\turtl\repos\PlaycastCompanionNative\build.cmd" check MyFile.cpp`
- Bash: `cmd.exe //c "C:/Users/turtl/repos/PlaycastCompanionNative/build.cmd" check MyFile.cpp`
  (double slash disables the conversion). Expect the first compile of a WinRT-heavy
  file to take ~1 minute; that's header cost, not a hang.
