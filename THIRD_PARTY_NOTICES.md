# Third-party notices

Playcast Companion bundles **no third-party code, fonts, or artwork**. It talks
to other software over their documented local interfaces. Using it with a given
ecosystem means you also accept that ecosystem's own terms:

| Integration | How the app talks to it | Terms you accept by using it |
|---|---|---|
| Razer Chroma | Local REST API (`localhost:54235`) | Razer Chroma SDK license / Synapse EULA |
| SteelSeries GameSense | Local REST API (address from `coreProps.json`) | SteelSeries GG EULA / GameSense SDK terms |
| Logitech LED Illumination SDK | Dynamic loading of a DLL **you** provide or that G HUB installs (not redistributed here) | Logitech SDK license |
| Corsair CUE SDK (v3) | Dynamic loading of a DLL **you** provide (not redistributed here) | Corsair iCUE SDK license |
| OpenRGB | Its TCP SDK protocol (port 6742). No OpenRGB code is linked or bundled, so OpenRGB's GPL does not apply to this project. | OpenRGB's own license governs OpenRGB itself |
| Windows Dynamic Lighting | Public WinRT `Windows.Devices.Lights` API | Windows license |
| Discord Rich Presence | Discord's local IPC (named pipe) with **your own** Discord Application ID | Discord Developer Terms of Service |
| Discord app registry | Unauthenticated `GET /api/v9/applications/{id}/rpc` (name lookup only, cached) | Discord Developer Terms of Service |

## Linked components
The native build links only Windows SDK components — C++/WinRT headers
(`Windows.Devices.Lights`, `Windows.Data.Json`), WinHTTP, GDI+, Winsock, and
the Win32 API — plus the Microsoft Visual C++ standard library, statically
linked. No third-party libraries are linked, vendored, or downloaded at build
time.

## Fonts
The UI uses Windows system fonts (Segoe UI, Bahnschrift, Consolas) and refers to
**Roboto** by name only if it is already installed on the machine (Roboto is
Apache-2.0; it is not bundled). No font files ship with this repository.

## Trademarks
Razer, Chroma, Synapse, SteelSeries, GameSense, Logitech, Logitech G, Corsair,
iCUE, OpenRGB, Windows, Dynamic Lighting, Discord, and Playcast are trademarks
or registered trademarks of their respective owners. They are used here only to
identify which products this software can interoperate with (nominative use).
This project is **not** affiliated with, endorsed by, or sponsored by any of
them. The repository intentionally ships **no** logo artwork; the cards use
original drawn marks and brand colours.
