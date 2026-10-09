# Session logging and local analysis

**Session logs** starts capture automatically for real sessions of the configured
guest account. Captures live under
`%LOCALAPPDATA%\PlaycastCompanion\sessions` in owner/SYSTEM-restricted folders.
Each session has separate `session`, `realtime`, `ipc`, `scripts`, `games`, and
`diagnostics` JSONL files. Defaults are a 30-second snapshot interval, 10 MiB file
rotation, a 100 MiB session budget, and 14-day retention; change them in Settings.

Launcher discovery resolves the configured guest's current SID/profile, including
Windows profile suffixes. Steam starts at the guest's
`AppData\Local\Playcast\Steam` and follows guest library/registry metadata.
Epic reads guest-registered manifests and Legendary's guest `installed.json`.
Shared Epic entries without guest registration must point into the guest profile
or Playcast game storage. The owner account's Steam library is never substituted.
Protected guest directories remain explicit coverage gaps. Junctions are resolved
when matching installation roots to process paths.

The snapshots include detected Steam/Epic installations, game processes matched
by installation root and guest session, Playcast/Companion resource use, disk
space, session and registry state, and lighting/Discord status. This is passive
inventory: other launchers, protected processes, and inaccessible sessions may
be missing. Coverage and limitations are recorded; unavailable inventory is not
reported as zero games.

Each session also gets terminal machine/game records (`phase: sessionEnd`),
including sessions shorter than the regular interval. Capture runs after backend
cleanup with a two-second budget; timeout or failure produces fresh Companion
resource observations with unknown inventory. A single pending capture prevents
overlapping blocked provider calls. Inaccessible Playcast service processes remain
listed with null metrics, error details and partial coverage.

Full realtime bodies and PowerShell content require a Playcast build containing
the Companion diagnostic hook. The stock host does not expose this feed. The
hook source is available as an [applyable patch](https://github.com/TypicalTitan/playcast-apps/tree/codex/companion-session-diagnostics)
in the personal Playcast repository; it still needs a host build. The authenticated
local pipe captures both directions and related provisioning/execution results.
Keep owner Companion running for that feed. The headless guest instance captures
its own passive snapshots under the guest profile without opening a second pipe.

The host hook does not re-enable development-only native log files in stock
Playcast Release binaries. PlaycastService, Playjector, PlayjectorHelper and
ElevationPolicyAgent skip file-logger initialization, and shared Warning/Info/Debug
calls are compiled out. Full native logging needs updated Playcast binaries with
a logging-only runtime setting. Companion's raw-secrets checkbox changes record
redaction; it does not enable native logging or developer mode.

Credentials are redacted by default. **Include raw secrets for this app run**
changes future records immediately and resets when the process restarts; it is
never saved as enabled. Arbitrary script output may contain sensitive text even
with normal redaction. Imported records retain the policy used when captured.

In **Analyze logs**, import `.jsonl`, `.ndjson`, or `.log` files, import a session
folder, drop files into the window, or load the included synthetic sample. Choose
**Timeline**, **Communications**, **Scripts**, **Games**, or **Health** to inspect
events, correlated scripts/results, latest inventory, or memory/CPU plots.
Select a row to read its original fields and source file/line. CPU rates require
comparable consecutive samples; missing metrics remain unknown.

Imports are local and never execute scripts or upload records. Bounds are 128 MiB,
50,000 valid records, 100,000 scanned lines, 10 MiB per record, and 5,000 discovered
files. Folder discovery stops after 100,000 entries and skips folders deeper than
32 levels. Malformed records produce warnings and partial results remain visible.
