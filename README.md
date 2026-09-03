# QoiShellExt

In-process Explorer shell extensions for [QOI](https://qoiformat.org/) images:
a thumbnail provider and a preview-pane handler for `.qoi` files.

*[Leia em português](README.pt-BR.md)*

## What it does

- **Thumbnails** in File Explorer, at any icon size.
- **Preview pane** rendering, scaled to fit, with the pane's own background and
  a checkerboard behind transparency.

Both run inside the shell's own host process (`dllhost.exe` and
`prevhost.exe`). There is no helper executable to launch, no temp file and no
managed runtime to start, so a thumbnail costs about half a millisecond instead
of the time it takes to boot a process.

The two handlers are independent: install one, the other, or both.

## Measured

| file | size | target | time |
| --- | --- | --- | --- |
| small image | 520x256 | 256 px thumbnail | 0.49 ms |
| small image | 520x256 | 500x400 preview pane | 0.49 ms (`DoPreview`) |
| 4000x3000 gradient (18 MB) | 12 Mpx | 256 px thumbnail | 79 ms |
| 4000x3000 gradient (18 MB) | 12 Mpx | 1024 px thumbnail | 88 ms |

The large case is dominated by the QOI decode itself, which is inherently
sequential - there is no way to decode only part of the image.

## How it works

One DLL, two CLSIDs:

| | CLSID | shellex |
| --- | --- | --- |
| Thumbnail provider | `{7BF3DDA8-F6F6-49E0-8F62-7B15E312302C}` | `{E357FCCD-...}` |
| Preview handler | `{BDB2367F-2BC8-4503-A870-AF02BD359A65}` | `{8895b1c6-...}` |

- `QoiImage.cpp` - decode through the reference `qoi.h` (`QOI_NO_STDIO`, forced
  to 4 channels) and scaling: box filter when minifying, bilinear when
  magnifying, both weighting color by alpha so transparent pixels do not bleed
  a dark halo into the edges.
- `ThumbnailProvider.cpp` - `IInitializeWithStream` + `IThumbnailProvider`,
  writing straight into a top-down 32 bpp DIB section. Never enlarges; returns
  `WTSAT_ARGB` (straight alpha, which is what QOI stores). `ThreadingModel =
  Both`, so the thumbnail host does not have to marshal.
- `PreviewHandler.cpp` - `IPreviewHandler` + `IPreviewHandlerVisuals` +
  `IOleWindow` + `IObjectWithSite` over a plain child window. Scales to fit
  (enlarging when the image is small), honours the pane's background and text
  colors, draws a checkerboard behind transparency, caches the scaled bitmap
  per pane size and double-buffers so dragging the splitter does not flicker.
  `ThreadingModel = Apartment`, `AppID` set to the `prevhost.exe` surrogate.
- `ShellExt.cpp` - class factory, `DllRegisterServer` / `DllUnregisterServer`
  and `DllInstall`, which is what makes per-handler installation possible.

Dependencies: Win32 only (`gdi32`, `msimg32`, `ole32`, `advapi32`, `shlwapi`,
`shell32`). Built with `/MT`, so no VC++ redistributable is needed.

## Build

```bat
build.cmd
```

Produces `build\QoiShellExt.dll` (x64). Needs Visual Studio 2022 Community at
the default path; adjust `VSDEV` in the script otherwise.

`build.cmd thumbnail` and `build.cmd preview` compile a DLL that carries only
that handler. You rarely need them: the default build contains both, and which
ones get *registered* is decided at install time.

## Install

Grab the zip from the Releases page, extract it and run `install.cmd` as
administrator - no compiler, no build step. The same scripts work straight
from the source tree after `build.cmd`.

```bat
install.cmd             (as administrator - both handlers)
install.cmd thumbnail   (thumbnails only)
install.cmd preview     (preview pane only)

uninstall.cmd           (removes everything)
uninstall.cmd preview   (removes just that handler)
```

Installs to `%ProgramFiles%\navossoc\QoiShellExt`. Adding the other handler
later is another `install.cmd`, no rebuild.

Under the hood this is `regsvr32 /n /i:<handler>`, which calls `DllInstall`
instead of `DllRegisterServer`. Plain `regsvr32 QoiShellExt.dll` still installs
everything the DLL carries.

If some other `.qoi` thumbnail or preview handler is already registered,
disable it first - many of them re-register themselves at startup and take the
association back.

To see the change, clear the thumbnail cache:

```bat
del /q "%LocalAppData%\Microsoft\Windows\Explorer\thumbcache_*.db"
```

## Test

```bat
test\build-test.cmd
test\build-gen.cmd
cd build
gen.exe sample.qoi 640 480
test.exe thumb   sample.qoi 256 200
test.exe preview sample.qoi 500 400
```

`test.exe` loads the DLL without registering it in COM, drives the handler and
writes `out.bmp` for inspection. The preview mode paints through
`WM_PRINTCLIENT`, so no window has to be visible.

`gen.exe` writes a `.qoi` of any size; pass something like `4000 3000` to
measure the cost on a real-sized image.

## Release

```bat
package.cmd
```

Builds both handlers and stages `dist\QoiShellExt-<version>-x64.zip` with the
DLL, both install scripts, the license and the READMEs, then prints the
artifact's SHA256 for the release notes. The version comes from `VER_STRING`
in `QoiShellExt.rc`, so that is the only place to bump it.

## License

MIT - see [LICENSE](LICENSE). Bundles `qoi.h` by Dominic Szablewski, also MIT.
