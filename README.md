My fork of TUS's Gearmulator project, where I add emulations of Elektron's
Machinedrum and Monomachine.

I'm not affiliated with TUS or Elektron. Don't bug them for support :)

There is a Discord channel [here](https://discord.gg/BnkTKpmp8) at #gearmulator-development.

[Downloads](https://github.com/joelanders/gearmulator-md-mm/releases) ·
[Report a bug](https://github.com/joelanders/gearmulator-md-mm/issues)

## Using the panel

- **Key chording / p-locks:** shift-click one or more buttons to hold them
  down until you release the shift key.
- **Secondary functions:** rather than shift-click Function and another button,
  you can just click the secondary function text label.
- **Encoder clicking:** Alt/Option-click a DATA ENTRY encoder to press it, or
  Alt/Option-drag to press and turn. With a trig held, pressing its parameter's
  encoder toggles that parameter lock. This applies to encoders A–H, not LEVEL
  or SOUND SELECTION.
- **Send SysEx File** under the right click menu to send a `.syx` file to the
  machine. The menu shows transfer progress and lets you cancel. Follow the
  machine's normal receive procedure.
- **Panel look and feel:** adjust encoder-drag and mouse-wheel sensitivity in settings.
  An experimental crisp LCD/panel rendering option is also available.
- **Audio inputs and outputs:** route host audio to the machine's input effects or sampling
  functions. Additional output pairs are available in a multi-output VST3 host;
  the standalone apps use stereo output.

Thanks to the upstream Gearmulator contributors whose work makes this fork
possible. See [the upstream README](README.upstream.md) for the original project
overview.
