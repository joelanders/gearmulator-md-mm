# Changing MD/MM tooltip text without rebuilding

The hover tooltips for knobs, LCD fields and machine names have built-in text: see
`source/elektron/md/mdJucePlugin/mdParameterHelp.h`, `mdMonomachineHelp.h` and `mdMachinedrumHelp.h`.
To try new wording, you don't need to edit those headers or rebuild.

1. Open the plug-in once. It writes `tooltips-defaults.txt` to its data folder, the folder that also
   holds its `logs` directory. The Machinedrum and the Monomachine each have their own folder.
2. That file lists every tooltip string as `key = text`, one per line:

   ```
   mm.amp.ATK.description = How long the amp envelope takes to rise to full level.
   mm.machine.SID-6581.PW.name = Pulse width
   md.trx.DEC.description = How long the sound takes to fade out.
   ```

3. Copy the lines you want to change into `tooltips.txt` in the same folder, and edit them there.
   While the plug-in is running, the change shows up the next time a tooltip updates. Hover away and
   back to see it.

Lines you leave out keep their built-in text, and deleting `tooltips.txt` restores everything. Lines
starting with `#` are comments. A key that matches nothing is ignored.
`tooltips-defaults.txt` is rewritten whenever the built-in text changes, so don't edit it.

Once the wording is right, move it into the headers so everyone gets it. The key names the table
entry:
- `mm.<page>.<label>`: fixed Monomachine pages (`amp`, `filter`, `effects`, `lfo`)
- `mm.machine.<LCD name>.<label>`: SYNTHESIS parameters
- `md.<family>.<label>`: Machinedrum machine families
- `.name` / `.description` / `.meaning`: which field

Keep the wording your own. Paraphrase the owner's manual rather than copying it.
