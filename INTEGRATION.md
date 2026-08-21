# Integrating Playcast Companion into Playcast

This document is for Playcast (or any host product) engineers evaluating
whether to adopt this behavior natively. Short version: **the hard parts are
already isolated behind one small interface, they are plain C++20 over Win32 /
WinHTTP / Winsock / C++/WinRT with no third-party dependencies, and the product
already knows the one fact this app spends most of its code discovering.**

## What the app does, in one sentence

While a specific Windows account has a session (your guest account), hold
every RGB ecosystem at black and show a Discord presence; when the session
ends, hand everything back.

## Where the seams are

```
SessionWatcher      -> "is the guest logged on?"  (WTS notifications + enumeration)
ILightingBackend    -> one hold/release loop per ecosystem  (6 implementations)
DiscordPresence     -> Discord Rich Presence over local IPC (named pipe)
DiscordShim         -> optional guest-session shim that learns which game is running
TrayApp             -> orchestration + tray/settings UI (Win32 + GDI+)
```

`ILightingBackend` (`src/LightingBackend.h`) is the seam that matters:

```cpp
class ILightingBackend {
public:
    virtual ~ILightingBackend() = default;
    virtual std::wstring DisplayName() const = 0;
    virtual bool Enabled() const = 0;
    virtual bool IsHolding() const = 0;
    virtual std::wstring StatusText() const = 0;
    virtual void StartBlackout() = 0;
    virtual void StopBlackout() = 0;   // blocks until the hold loop has released
};
```

`LightingBackendBase` supplies the retry/heartbeat loop on a `std::jthread`;
each backend is a thin protocol adapter:

| Backend | Implemented with | Dependencies |
|---|---|---|
| Razer Chroma | WinHTTP REST to `localhost:54235` | none |
| SteelSeries GameSense | WinHTTP REST, address from `coreProps.json` | none |
| OpenRGB | Winsock TCP, OpenRGB SDK protocol on `:6742` | none |
| Logitech G | `LoadLibrary` of the LED Illumination SDK DLL | DLL supplied by G HUB / you |
| Corsair iCUE | `LoadLibrary` of CUE SDK v3 DLL | DLL supplied by you |
| Windows Dynamic Lighting | C++/WinRT `Windows.Devices.Lights.LampArray` | Windows SDK headers only |

JSON goes through a tiny wrapper over `Windows.Data.Json` (`src/Json.h`), HTTP
through a tiny wrapper over WinHTTP (`src/Http.h`). **You don't need
`SessionWatcher` at all** — your GuestModeManager already knows the exact moment
guest mode enters and tears down, which is strictly better than an outside
observer watching WTS.

## Three integration options (cheapest first)

### 1. Sidecar process (near-zero code change)
Ship the companion exe alongside the host and have the service launch it with a
control flag at guest enter/teardown. Today the app auto-detects the session;
adding an explicit `--stealth on|off` mode that forces hold/release (ignoring
WTS) is a ~50-line change in `TrayApp.cpp`/`main.cpp`. Pros: no porting, a
single ~1 MB static exe with no runtime, isolated failure domain, MIT-licensed
code you can vendor. Cons: a second process to deploy and keep in sync.

### 2. Drop the backends into your existing C++ process
Every backend is plain C++20 over the Windows SDK — no .NET, no vendored
libraries, no package manager. Copy `LightingBackend.{h,cpp}`, the controllers
you want, and the `Json`/`Http`/`Log` helpers into your tree, compile them with
`/std:c++20` against the Windows 10 SDK (10.0.22621+ for C++/WinRT
`LampArray`), link `winhttp`, `ws2_32`, `windowsapp`, and call
`StartBlackout()` / `StopBlackout()` from `GuestModeManagerEnter` /
`GuestModeManagerTeardown`. Each backend is self-contained and under ~300
lines; the three REST/TCP ones have zero vendor-SDK surface. Pros: single
process, native feel, no port needed. Cons: you own the vendor-SDK edge cases
for Logitech/Corsair (documented in each backend's header comment and in
`README.md`). If the host is TypeScript/Electron, the REST/TCP backends are
still an afternoon each to re-express over `fetch`/`net`.

### 3. Build it as a static library
`CMakeLists.txt` builds one executable; splitting the `src/` backends into a
`STATIC` library target is a ten-line CMake change. Link it into the host and
construct the backends directly. The UI is separable (everything UI is in
`MainWindow.cpp`/`TrayApp.cpp`/`Branding.cpp`).

## Effort estimate
| Option | Engineering effort | Risk |
|---|---|---|
| Sidecar + `--stealth` flag | ~1 day incl. installer wiring | Low |
| Drop backends into a C++ host | ~1–2 days for all six (mostly wiring + build), hours for Chroma alone | Low–Medium (vendor quirks) |
| Static library | ~1 day | Low, requires MSVC / C++20 in the host |

## Licensing you'd inherit
- **This code:** MIT — vendor, modify, ship, no copyleft.
- **Vendor SDKs:** each ecosystem's own developer terms apply to *you* as the
  integrator (Razer Chroma SDK license, SteelSeries GameSense, Logitech LED SDK,
  Corsair iCUE SDK). This repo redistributes none of their binaries — the
  Logitech/Corsair DLLs are loaded at runtime only if present. For a
  commercial product, review each SDK's license — some require registration or
  attribution. See `THIRD_PARTY_NOTICES.md`.
- **OpenRGB:** this app speaks OpenRGB's network protocol and links no OpenRGB
  code, so OpenRGB's GPL does not reach the integrator.
- **Windows SDK / C++/WinRT:** the only things linked. Standard Windows SDK
  terms; nothing to attribute.
- **Discord:** Rich Presence needs *your own* Discord Application ID under
  Discord's Developer Terms. The optional game-pass-through shim emulates a
  Discord IPC endpoint for guest-session games; it's off by default and, for a
  commercial product, is the one piece worth a counsel review against
  Discord's terms before shipping (or simply leave it out — it's fully
  separable, `DiscordShim.cpp`).
- **Trademarks:** the repo ships no vendor logos. Playcast obviously controls
  its own branding and can rebrand freely.

## Things to keep if you port
- Distinguish a *timeout* from a *stop request* inside the hold loop: only a
  signalled `std::stop_token` ends the loop; an HTTP/socket timeout is a retry,
  not a stop (treating timeouts as "stop" silently kills the loop — this was a
  real bug once in the C# version).
- Chroma heartbeat < 15 s, re-apply every few seconds (Synapse fights back).
- Dynamic Lighting must skip devices a vendor engine already drives, or two
  engines fight over the same hardware (`ExcludeVendorOwnedDevices`).
- Make blocking I/O unblockable from `Stop()` (`CancelSynchronousIo`, closing
  the handle/socket, or an event wait) so teardown finishes within seconds.
- Any thread touching WinRT initialises a multi-threaded apartment first.
- Run unelevated; nothing here needs admin.
