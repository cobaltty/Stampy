#!/usr/bin/env bash
cd src/icons/

echo "Fixing all stroke and fill colors across SVGs..."

for file in *.svg; do
    [ -f "$file" ] || continue
    
    # 1. Remove embedded <style>...</style> blocks that override GTK styling
    sed -i -E 's/<style[^>]*>.*<\/style>//g' "$file"

    # 2. Convert all hex strokes (stroke="#222222", stroke="#000000", etc.) to currentColor
    sed -i -E 's/stroke="#[0-9a-fA-F]{3,6}"/stroke="currentColor"/g' "$file"
    sed -i -E 's/stroke:#[0-9a-fA-F]{3,6}/stroke:currentColor/g' "$file"

    # 3. Convert all hex fills to currentColor
    sed -i -E 's/fill="#[0-9a-fA-F]{3,6}"/fill="currentColor"/g' "$file"
    sed -i -E 's/fill:#[0-9a-fA-F]{3,6}/fill:currentColor/g' "$file"
done

echo "Done! Rebuilding..."
cd ../..
meson compile -C _build
