#!/usr/bin/env bash
set -e

ICONS_DIR="icons"

if [ ! -d "$ICONS_DIR" ]; then
    echo "Error: Directory '$ICONS_DIR' not found. Please run this script inside src/"
    exit 1
fi

echo "1. Cleaning temporary sed files..."
rm -f "$ICONS_DIR"/sed*

echo "2. Ensuring all SVG filenames end with '-symbolic.svg'..."
cd "$ICONS_DIR"

for file in *.svg; do
    [ -f "$file" ] || continue
    
    # Skip if already ends with -symbolic.svg
    if [[ "$file" =~ -symbolic\.svg$ ]]; then
        continue
    fi

    # Handle special name replacement for format_text-underline
    base="${file%.svg}"
    if [ "$base" = "format_text-underline" ]; then
        new_name="format-text-underline-symbolic.svg"
    else
        new_name="${base}-symbolic.svg"
    fi

    echo "   Renaming $file -> $new_name"
    mv "$file" "$new_name"
done

echo "3. Updating SVG fill attributes to 'currentColor'..."
for file in *.svg; do
    [ -f "$file" ] || continue

    # Replace fill hex attributes (#000000, #222222, etc.)
    sed -i -E 's/fill="#[0-9a-fA-F]{3,6}"/fill="currentColor"/g' "$file"
    
    # Replace inline style fill properties (style="fill:#000000...")
    sed -i -E 's/fill:#[0-9a-fA-F]{3,6}/fill:currentColor/g' "$file"
done

cd ..

echo "Done! All SVG files are now symbolic and support GTK4 dark mode."
