# Stampy

Batch-stamp hundreds of photos with compass overlays and text pulled from
your own data files. Made for field documentation workflows — e.g. stamping
site photos with coordinates, orientation, and notes — without editing them
one by one.

> Note: Stampy is a specialized tool built around compass/DOCX/XLSX
> field-survey workflows. If that is not your use case, it probably is
> not the app for you.

## What it does

- Overlay a compass rose per photo, auto-matched to each photo's azimuth.
- Overlay watermark text per photo from a `.docx` file (matched by photo id).
- Overlay coordinates and heading from an `.xlsx` geolocation table.
- Export in parallel with original EXIF preserved and GPS + heading
  written back via ExifTool.

## Input formats

- Compass: a folder of compass images named by degree (e.g. `90.png`).
- Watermark: a `.docx` with one block per photo starting with the photo id
  line; up to 6 following lines become the text, the 2nd line under the id
  is the azimuth.
- Geolocation: an `.xlsx` with columns A=id, B=longitude, C=latitude,
  D=azimuth.

## Install

```sh
flatpak remote-add stampy https://cobaltty.github.io/Stampy/com.cobaltty.stampy.flatpakrepo
flatpak install stampy com.cobaltty.stampy//stable
```

Updates arrive through normal `flatpak update`. Releases are GPG-signed;
if you installed while the remote was unsigned, remove and re-add it once
to switch to verified updates.

## Disclaimer

Stampy is still under active development by an owner who is still learning.
Expect bugs, rough edges, and breaking changes. use it at your own risk.

Any help would be valuable!
