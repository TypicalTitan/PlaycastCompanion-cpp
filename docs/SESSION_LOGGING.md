# Session logging and local analysis

**Session logs** starts capture automatically for real sessions of the configured
guest account. Captures live under
`%LOCALAPPDATA%\PlaycastCompanion\sessions` in owner/SYSTEM-restricted folders.
Each session has separate `session`, `realtime`, `ipc`, `scripts`, `games`, and
`diagnostics` JSONL files. Defaults are a 30-second snapshot interval, 10 MiB file
rotation, a 100 MiB session budget, and 14-day retention; change them in Settings.

The snapshots include detected Steam/Epic installations, game processes matched
by installation root and guest session, Playcast/Companion resource use, disk
space, session and registry state, and lighting/Discord status. This is passive
inventory: other launchers, protected processes, and inaccessible sessions may
be missing. Coverage and limitations are recorded; unavailable inventory is not
reported as zero games.

Full realtime bodies and PowerShell content require a Playcast build containing
the Companion diagnostic hook. The stock host does not expose this feed. The
hook source is available as an [applyable patch](https://github.com/TypicalTitan/playcast-apps/tree/codex/companion-session-diagnostics)
in the personal Playcast repository; it still needs a host build. The authenticated
local pipe captures both directions and related provisioning/execution results.
Keep owner Companion running for that feed. The headless guest instance captures
its own passive snapshots under the guest profile without opening a second pipe.

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
