# Crisp panel rendering experiment

`PixelPerfectPanel` owns the experiment's state, helper elements and LCD drawing
policy. Its only editor hooks are `applyPixelPerfectPanel()` and the optional
`paintLcd()` call before normal aspect-fit drawing. It has no audio/DSP, firmware,
panel-input or state-chunk dependencies.

The config key and default live in `mdPixelPerfectPanel.h`. The checkbox and editor
both use those constants, so changing the default cannot make their states disagree.
Explicitly saved choices take precedence. The experiment defaults off.

Only static, flat rules marked `elektronPixelRule` in the bundled skins participate.
Unmarked 1dp elements are untouched. The controller tracks helpers with RML observer
pointers so deleting a marked element does not leave a dangling pointer. Destruction
restores original backgrounds, removes helpers and disables its canvas/density options.
The controller must be destroyed before its RML core, as it is in `Editor`.

## Promote

Change `PixelPerfectPanel::defaultEnabled` and the experimental checkbox wording.
Keep the opt-out initially and preserve explicit saved choices. Before promotion,
validate GPU output, actual monitor migration and rendering CPU cost on older Intel
machines. At 2x density, software rendering covers four times as many pixels.

## Remove

Remove the two editor hooks, controller member/forward declaration, settings binding
and checkbox markup; remove `mdPixelPerfectPanel.{h,cpp}` and its CMake entry.
The `elektronPixelRule` markers can also be removed (they have no style or behavior
by themselves). Existing saved config keys become inert and need no migration.
Remove the experiment-specific parts of `mdPanelRenderingTest` with the controller.

Keep the independent LCD aspect-ratio correction and stable settings-template
suffix fix. Neither depends on `PixelPerfectPanel`. Shared canvas pixel alignment,
optional backing-density support and configurable checkbox defaults are reusable
capabilities with existing behavior as their default; they can remain without an
MD/MM caller. The queued-frame scale fix is independently useful and should stay.

## Verification

`mdPanelRenderingTest` is a firmware-free software-renderer integration test. It
checks already queued frames during toggles/density changes, exact integer LCD
blocks, restoration of baseline pixels, pointer mapping, padded-texture fallback,
explicit rule selection, DOM removal and controller teardown. The macOS VST3 CI
job builds and runs it. Full MD/MM embedded-skin comparisons are additionally
recorded in the local durable review notes; GPU screenshots are not covered here.
