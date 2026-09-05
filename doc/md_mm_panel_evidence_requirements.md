# MD/MM panel replacement: evidence requirements

Prepared 2026-09-05. The remaining MD `panelDisplayReadyPost()` operation writes
private firmware task-list state. Removing it still fails first-run UW flash
initialization after the HI08 receive and UART status/unmask corrections. A
replacement must cause progress through the emulated external interface, with
the private write absent. Existing emulator-generated replies are not an
independent reference for deciding which reply is correct.

## Public-source check

The following sources were checked on 2026-09-05:

- [Elektron's MKI service information](https://support.elektron.se/support/solutions/articles/43000566531-machinedrum-monomachine-mki-service-information)
  discusses service/spares but supplies no panel protocol or readiness exchange.
- The [current MAME driver](https://github.com/mamedev/mame/blob/master/src/mame/elektron/elektronmono.cpp)
  is a skeleton without the local startup-reply or task-list implementation.
  Its comments mix hardware observations with firmware-derived guesses; the
  file is not evidence of clean-room provenance for our implementation.
- Searches for MD/MM panel UART protocols, panel schematics, and the existing
  readiness terminology did not locate an independent exchange specification.
  This is limited negative evidence, not proof that no source exists. Public
  MIDI/TurboMIDI documentation does not specify this internal board link.

## Useful evidence to supply

Either an independently documented panel-controller interface with permission
to use it, or observations from a real device obtained through a method the
owner has authorized and is comfortable using, could establish the missing
behavior. Record:

- Source/author, date, acquisition method, and any restrictions on use/sharing.
- Exact MD/MM model, board revision if known, OS version, and capture conditions.
- Both directions of the controller link, timestamps, framing/baud information,
  and any independently identified reset/ready/flow-control signal transitions.
- An ordinary cold boot through stable display, idle activity, and a small
  reproducible panel interaction such as entering/leaving the tempo menu.
- A contemporaneous display/event log and a repeat run, to distinguish startup
  descriptors, periodic notifications, and responses to individual commands.

Do not erase user storage or invoke a factory reset merely to obtain a trace.
This document does not identify safe probe points or voltage levels: those
require verified board information and appropriate electronics expertise.
Firmware disassembly, private-memory dumps, and guessed acknowledgement bytes
are not substitutes for the requested external-interface evidence.

## Replacement acceptance

Derive a documented state/event model from the supplied evidence, preserving
its source and uncertainty. Add source-linked protocol fixtures and reset,
startup, idle, and panel-event tests. Then require MD first-run UW initialization,
continued LCD/LED progress, RAM audio, and MM boot/panel/audio regressions with
the private task-list write absent. Keep external panel protocol emulation
separate from MCU instruction execution; moving the private write into a panel
class would not satisfy this requirement.

This is an engineering evidence checklist, not a determination of legal
permissibility. See [the remediation journal](md_mm_firmware_hook_remediation.md)
for the retained code, test results, and remaining transport/interpreter risks.
