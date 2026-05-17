# DeeVee Smoke Testing

This checklist validates the current scaffold before DeeVee has real DVD sector reading or playback. Passing these tests means the core can classify ISO/CHD content paths, build a libretro DLL, and run placeholder AV output without crashing.

## Build

From an MSYS2 UCRT64 shell:

```sh
../.cursor/projects/empty-window/DeeVee
make clean
make
make test
make probe
```

Expected outputs:

- `deevee_libretro.dll`
- `tests/test_content.exe`
- `tools/deevee_probe.exe`

## Probe Local Samples

Run the probe against one ISO image and one CHD image:

```sh
./tools/deevee_probe \
  "/path/to/disc.iso" \
  "/path/to/disc.chd"
```

For the ISO sample, expected output includes:

```text
accepted: yes
detected_type: DVD ISO
```

For the CHD sample, expected output includes:

```text
accepted: yes
detected_type: DVD CHD
```

At this stage, ISO and CHD are accepted by path/extension only. DeeVee does not yet inspect sectors, UDF, CHD metadata, MPEG-2 video, subpictures, or audio streams.

## RetroArch Manual Smoke Test

1. Copy or point RetroArch at `deevee_libretro.dll`.
2. Load each sample path with the DeeVee core:
   - ISO image
   - CHD image
3. Confirm the placeholder video frame appears.
4. Confirm reset does not crash.
5. Confirm unload/reload does not crash.
6. Confirm D-pad, A, B, Start, and X input do not crash the core.

## Results To Capture

For each sample, record:

- sample format: ISO or CHD
- probe accepted: yes/no
- probe detected type
- RetroArch load result
- reset/unload/reload result
- notes or crash details

The next implementation milestone should start after both sample types pass this smoke layer.
