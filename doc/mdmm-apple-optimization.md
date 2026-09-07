# MD/MM Apple compiler optimization

MD/MM provides opt-in compiler settings for Release builds on macOS. The
default configuration uses the existing compiler settings. The options apply
to shared emulation libraries, so other products linked to those libraries
also consume the selected build.

| CMake option | Default | Effect |
| --- | --- | --- |
| `GEARMULATOR_MDMM_APPLE_THINLTO` | `OFF` | Build `mdLib` and `68kEmu` with ThinLTO and propagate the option to their final executable/plugin links. |
| `GEARMULATOR_MDMM_APPLE_OPTIMIZE_DSP` | `OFF` | Extend the selected optimization to `dsp56kEmu` and `dsp56kBase`. |
| `GEARMULATOR_MDMM_APPLE_PGO_MODE` | `none` | Select `none`, `generate`, or `use` for profile-guided optimization. |
| `GEARMULATOR_MDMM_APPLE_PGO_PROFILE` | Empty | Path to the merged profile for `use` mode. |

Add these options to the normal MD/MM build configuration. PGO requires
ThinLTO and one explicit `CMAKE_OSX_ARCHITECTURES` value. Train and build each
architecture separately. ThinLTO alone can also be used in a universal build.
The flags apply only to Release configurations.

## Profile generation and use

1. Pin the parent and submodule revisions, compiler, architecture, and build
   definitions. Keep a record of these and the workload used for training.
2. Configure an instrumented Release build with
   `GEARMULATOR_MDMM_APPLE_THINLTO=ON` and
   `GEARMULATOR_MDMM_APPLE_PGO_MODE=generate`. Select the same DSP option for
   generation and use.
3. Create an empty profile directory. Launch the instrumented executable
   directly with `LLVM_PROFILE_FILE` set to an absolute path such as
   `/path/to/profiles/arm64-%p.profraw`. Exercise representative workloads in
   both instruments, including the buffer sizes and sample rates being
   targeted. Quit cleanly to flush the profile. Instrumented builds have
   additional CPU overhead and are intended for training.
4. Merge those files with the matching compiler's tool:
   `xcrun llvm-profdata merge -o /path/to/arm64.profdata /path/to/profiles/*.profraw`.
5. Configure a separate Release build with the same options and source,
   `GEARMULATOR_MDMM_APPLE_PGO_MODE=use`, and
   `GEARMULATOR_MDMM_APPLE_PGO_PROFILE=/path/to/arm64.profdata`.
6. Verify audio, emulated timing, and callback performance against an ordinary
   Release control before using the result as a release candidate. A profile
   specialized to one workload can hurt another workload.

Replacing the merged profile at the same path automatically rebuilds the
optimized Release libraries when the profile contents change.

Profile-use builds treat Clang's out-of-date profile diagnostics as errors.
Regenerate the profile when source, compiler, architecture, or relevant
definitions change; some function-hash mismatches can be treated as missing
data without an out-of-date warning. A successful build does not establish
that an old profile still represents the intended workload. An unprofiled-file
warning can occur for translation units not reached by the training executable,
or when none of a file's function hashes match. Review these warnings for stale
profiles as well as gaps in training; this compiler diagnostic alone cannot
validate a profile.

Keep generated `.profraw` and `.profdata` files as local build inputs. The
resulting optimized executable does not require the profile at runtime.
