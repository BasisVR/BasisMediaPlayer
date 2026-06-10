# Third-party statics (RIST) — built from source

These are **build inputs**, not Unity plugins. They are statically linked into
`basis_media_native` when the plugin is built with `-DBASIS_WITH_RIST=ON`; they are
never loaded by Unity directly and never shipped as separate plugins. They live under
`Native~/` (the trailing `~` hides the folder from Unity's importer), so no `.meta`
files apply.

This is deliberately different from how Opus ships (`Plugins/<rid>/native/lib*`):
Opus is a managed P/Invoke library whose native libs are runtime-loaded, so they belong
in `Plugins/`. librist is linked into our own native plugin at build time, so its
archives are build inputs here instead.

## Layout

```
third_party/
  include/
    librist/               # librist.h, peer.h, receiver.h, …
  win-x64/                 # Windows x86_64
    rist.lib               #   librist (mbedTLS bundled inside)
  android-arm64/           # Android arm64-v8a
    librist.a              #   librist (mbedTLS bundled inside)
```

librist vendors its own mbedTLS and links it into the archive, so a single library per
platform is all that's needed — the consumer links only `rist`. `CMakeLists.txt` resolves
it via `find_library(rist)`, matching each platform's convention (`rist.lib` on Windows,
`librist.a` on Android). If it's missing the CMake configure step fails with a pointer back
here, rather than producing a half-linked plugin.

Runtime-identifier names (`win-x64`, `android-arm64`) match the convention Basis already
uses for Opus's per-platform binaries. Windows and Android are the first targets; add a
new `<rid>/` directory and a branch in the `BASIS_WITH_RIST` block of `CMakeLists.txt`
when another backend gains a real decode path.

## Producing the archives

librist is built **from source**, never committed here. Run the build script for your
host — it clones librist at the pinned tag (matching what `basis_rist.c` targets), builds
it static with Meson (its bundled mbedTLS linked in), and stages the result into the
layout above:

```
# Windows x64 — from a Developer prompt (MSVC on PATH), with: pip install meson ninja
Native~/build-librist.ps1

# Android arm64 — needs ANDROID_NDK_ROOT, plus meson + ninja
Native~/build-librist.sh android-arm64
```

CI runs the same scripts (`.github/workflows/media-native.yml`) on every change under
`Native~/`, so the recipe stays green and the statics are downloadable as build artifacts.
The build matches the plugin's toolchain: `/MD` CRT on Windows (the `basis_media_native`
default), NDK `arm64-v8a` / `android-29`+ on Android. To pin a different librist version,
pass the tag through (`-LibristRef` / `LIBRIST_REF`), kept in lockstep with the API
`basis_rist.c` targets.

The plugin binary itself (`basis_media_native.{dll,so}`) is still built on a
Unity-equipped machine — it links the editor's PluginAPI headers, which CI doesn't have.
So the role here is to keep librist building from source, not to produce the plugin. On
Windows, `Native~/build-win-x64-rist.ps1` chains all of it (librist → plugin → install
into `Plugins/Windows/x86_64/`) in one command.

## Licensing

Both are permissive and MIT-compatible (mbedTLS is bundled inside librist), and both carry
attribution obligations recorded in the package's `THIRD_PARTY_NOTICES.md`:

- **librist** — BSD-2-Clause
- **mbedTLS** — Apache-2.0
