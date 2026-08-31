# Machinedrum and Monomachine SysEx

No firmware, factory NVRAM, SysEx backup, preset library, firmware-derived
fixture, or private test capture is included or downloaded. Users must supply
files they are entitled to use.

## OS updater files

The emulator accepts an official Machinedrum or Monomachine OS-update `.syx`
directly. Put the update in the matching legacy data directory used by the
current builds (`Machinedrum/roms` for Gearmulator MD or `Monomachine/roms` for
Gearmulator MM), then restart the plug-in or Standalone application. No external
`.bin` conversion is needed. A matching 8 MiB `.bin` dump remains supported and
takes precedence when both are present.

The SysEx is decoded into a private Gearmulator image in memory only. That image
is not a hardware flash dump and must never be sent to a device.

For Monomachine, the updater's factory-data section also supplies the initial
storage image when no valid `nvram/mm-factory-live3-be.bin` is installed. An
installed or restored NVRAM image still takes precedence. The official OS
updater does not contain the separate MKII factory DigiPRO waveform bank, so
those factory waveforms remain unavailable when booting from `.syx` alone.

## Implementation boundary

The OS-update reader implements the legacy SysEx packet framing plus the update
container's compressed sections. Its transport decoder and aPLib variant are
based on Marcel Bierling's MIT-licensed
[`elektron-firmware-tool`](https://github.com/mischa85/elektron-firmware-tool);
the required license text is shipped beside the implementation. Gearmulator
adds a private in-memory image layout and direct initialization of the emulated
ColdFire and DSP hardware. It does not reconstruct or export a hardware flash
dump.
