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

Build and install the Flatpak with `com.cobaltty.stampy.json`.
