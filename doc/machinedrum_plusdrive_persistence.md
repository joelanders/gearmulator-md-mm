# Machinedrum +Drive persistence

The emulated Machinedrum has two intentionally different persistence owners:

- A plug-in instance keeps its +Drive inside the host project state. It never
  writes an implicit external mirror, so two projects cannot overwrite one
  another's machine data.
- The standalone application keeps one fixed checkpoint in the Machinedrum user
  data directory. One process owns that file for its whole session. Additional
  standalone processes are read-only and explain that they must be restarted
  after the writer closes before they can persist changes.

On the first launch after upgrading, the standalone imports the +Drive contained
in JUCE's existing `filterState` setting if no dedicated checkpoint exists. Once
the checkpoint has been created, it is authoritative and a stale `filterState`
cannot roll it back on later launches.

Standalone writes settle for about one second and are then atomically replaced.
A clean quit performs a final flush. A forced termination during the settling
window may lose only the newest unsettled change; it must leave the preceding
checkpoint valid. An invalid checkpoint is preserved and blocks automatic writes
until the user explicitly imports a valid image or creates a blank +Drive.

## State-size bound

The sparse MDPD format uses 16 bytes plus 516 bytes for each stored 512-byte
sector. Its defensive serialized-image limit is 512 MiB. The largest valid image
below that limit contains 1,040,447 records and is 536,870,668 bytes. With the
1 MiB battery-backed machine snapshot and the version-4 header, the corresponding
minimal-flash project payload is 537,919,304 bytes (just under 513 MiB), before
the small outer plug-in envelope.

That is a safety bound, not a recommended working size. Host-specific plug-in
state limits can be lower, and a heavily populated +Drive also requires multiple
in-memory copies while a project is saved or restored. Normal freshly formatted
and selectively populated sparse images are much smaller. Users approaching very
large images should export an `.mdpd` backup and verify the intended DAW's save and
reload behavior.

The opt-in `mdPlusDriveStateWorkload` target constructs the exact bounded maximum,
encodes it into project state, decodes it, and reports byte counts, timings, and
peak resident memory where the platform exposes it. It is deliberately not a
normal CTest because its purpose is to exercise the roughly 513 MiB boundary.

On the 2026-09-01 arm64 macOS Release validation run, the maximum workload encoded
in 4.245 seconds, decoded and verified in 4.298 seconds, and reported a peak
resident set of 1,639,645,184 bytes (about 1.53 GiB). These are diagnostic numbers,
not a cross-platform performance promise.
