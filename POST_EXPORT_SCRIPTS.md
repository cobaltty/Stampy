# Stampy Post-Export Scripts Guide

Stampy can execute an external script (Bash, Python, etc.) immediately after an export batch finishes.

---

## 1. How It Works

When export finishes, Stampy executes:

```bash
/path/to/script.sh "$EXPORT_FOLDER" [optional arguments]
```

Output (`stdout` and `stderr`) streams directly into the Stampy progress log panel.

---

## 2. Script Inputs

### Command-Line Arguments
- **`$1`**: Full path to the export destination directory (e.g. `/home/user/Surveys/PU1_stamped`).
- **`$2`, `$3`, ...**: Custom parameters entered in **Preferences -> Post-Export Automation -> Script arguments**.

### Environment Variables
- `STAMPY_EXPORT_FOLDER`: Identical to `$1`.
- `STAMPY_TOTAL`: Total image count queued.
- `STAMPY_SUCCESS`: Image count successfully exported.

---

## 3. Quickstart Templates

### Template A: Bash (`survey_hook.sh`)

```bash
#!/usr/bin/env bash
set -e

EXPORT_DIR="$1"
EXTRA_ARG="$2"

echo "[HOOK] Starting post-export processing on: $EXPORT_DIR"
echo "[HOOK] Total images: $STAMPY_TOTAL (Success: $STAMPY_SUCCESS)"

# ----------------------------------------------------
# Put custom tasks here (examples below)
# ----------------------------------------------------

echo "[HOOK] Post-export tasks finished."
```

Make it executable:
```bash
chmod +x ~/survey_hook.sh
```

---

### Template B: Python (`survey_hook.py`)

```python
#!/usr/bin/env python3
import sys
import os

export_dir = sys.argv[1]
extra_arg = sys.argv[2] if len(sys.argv) > 2 else ""

total = os.environ.get("STAMPY_TOTAL", "0")
success = os.environ.get("STAMPY_SUCCESS", "0")

print(f"[HOOK] Running Python automation on {export_dir}")
print(f"[HOOK] Processed {success}/{total} photos.")

# Put custom logic here (requests, pandas, PIL, zipfile, etc.)
```

Make it executable:
```bash
chmod +x ~/survey_hook.py
```

---

## 4. Common Recipes

### Recipe 1: Auto-Zip Exported Folder
Creates `PU1_stamped.zip` next to the export folder.

```bash
#!/usr/bin/env bash
TARGET="$1"
ZIP_DEST="${TARGET%/}.zip"

echo "Compressing $TARGET to $ZIP_DEST..."
zip -r -q "$ZIP_DEST" "$TARGET"
echo "Zip completed: $ZIP_DEST"
```

### Recipe 2: Interactive Prompt via GNOME Zenity
Prompts the operator with a native GUI dialog if client asks for on-the-fly input:

```bash
#!/usr/bin/env bash
TARGET="$1"

CLIENT_NOTE=$(zenity --entry --title="Survey Export" --text="Enter client note to append to metadata:")

if [ -n "$CLIENT_NOTE" ]; then
    exiftool -r -overwrite_original "-Comment+=$CLIENT_NOTE" "$TARGET"
    echo "Appended note to metadata."
fi
```

### Recipe 3: Move or Upload to Remote / Cloud Drive
Copies finished folder to a mounted Nextcloud / Google Drive / NAS:

```bash
#!/usr/bin/env bash
TARGET="$1"
BACKUP_DIR="/mnt/survey_share/Exports/"

mkdir -p "$BACKUP_DIR"
cp -r "$TARGET" "$BACKUP_DIR"
echo "Backed up $TARGET to $BACKUP_DIR"
```

---

## 5. Stampy Configuration

1. Open Stampy.
2. Click **Menu (top right) -> Preferences**.
3. Scroll to **Post-Export Automation**:
   - Turn **ON** `Run script after export`.
   - **Script path**: Absolute path to script (e.g. `/home/user/scripts/survey_hook.sh`).
   - **Script arguments**: Any optional flags (e.g. `--fast --notify`).
