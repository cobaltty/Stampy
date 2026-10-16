import os
import re
import argparse
from docx import Document
from PIL import Image, ImageDraw, ImageFont, ImageOps

def get_coords(pos, img_w, img_h, item_w, item_h, off_x, off_y):
    if pos == "UpLeft": return (int(off_x), int(off_y))
    if pos == "UpRight": return (int(img_w - item_w - off_x), int(off_y))
    if pos == "DownLeft": return (int(off_x), int(img_h - item_h - off_y))
    # Default: DownRight
    return (int(img_w - item_w - off_x), int(img_h - item_h - off_y))

def stamp_field_photos(args):
    if not os.path.exists(args.docx):
        print(f"[ERROR] Document file not found: {args.docx}", flush=True)
        return

    doc = Document(args.docx)
    text_content = "\n".join([p.text.strip() for p in doc.paragraphs if p.text.strip()])
    blocks = [b.strip() for b in re.split(r'(?=\b\d{4}\b)', text_content) if b.strip()]

    if not os.path.exists(args.output):
        os.makedirs(args.output)

    total_blocks = len(blocks)
    print(f"[INFO] Initializing engine... Found {total_blocks} item(s) to process.", flush=True)
    print(f"PROGRESS:0", flush=True)

    for idx, block in enumerate(blocks, start=1):
        lines = block.split('\n')
        if not lines:
            continue

        file_id = lines[0].strip()
        photo_path = os.path.join(args.photos, f"{file_id}.jpg")

        # Fall back to other extensions when .jpg is missing.
        if not os.path.exists(photo_path):
            for ext in [".png", ".jpeg", ".JPG", ".PNG", ".JPEG"]:
                alt_path = os.path.join(args.photos, f"{file_id}{ext}")
                if os.path.exists(alt_path):
                    photo_path = alt_path
                    break
            else:
                print(f"[WARNING] [{file_id}] SKIPPED: Source image not found.", flush=True)
                # Still report progress for skipped files.
                percent = int((idx / total_blocks) * 100)
                print(f"PROGRESS:{percent}", flush=True)
                continue

        try:
            # 1. Open base photo with EXIF orientation applied.
            img = ImageOps.exif_transpose(Image.open(photo_path)).convert("RGBA")
            img_w, img_h = img.size

            # 2. Overlay compass.
            compass_match = re.search(r'\d+°\s*[A-Z]+', block)
            if compass_match:
                target_clean = re.sub(r'\s+', '', compass_match.group(0)).replace('º', '°') + ".png"
                compass_filename = next((f for f in os.listdir(args.compass) if re.sub(r'\s+', '', f).replace('º', '°') == target_clean), None)

                if compass_filename and os.path.exists(os.path.join(args.compass, compass_filename)):
                    compass_img = Image.open(os.path.join(args.compass, compass_filename)).convert("RGBA")

                    # Scale to target width.
                    w_percent = (args.c_size / float(compass_img.size[0]))
                    h_size = int((float(compass_img.size[1]) * float(w_percent)))
                    compass_img = compass_img.resize((args.c_size, h_size), Image.Resampling.LANCZOS)

                    # Apply opacity.
                    r, g, b, a = compass_img.split()
                    a = a.point(lambda p: int(p * (args.c_opacity / 100.0)))
                    compass_img = Image.merge("RGBA", (r, g, b, a))

                    # Position.
                    c_x, c_y = get_coords(args.c_pos, img_w, img_h, compass_img.size[0], compass_img.size[1], args.c_x, args.c_y)
                    img.paste(compass_img, (c_x, c_y), compass_img)

            # 3. Overlay text.
            try:
                font = ImageFont.truetype(f"{args.t_font.lower().replace(' ', '')}.ttf", args.t_size)
            except Exception:
                font = ImageFont.load_default()

            text_to_draw = "\n".join(lines[1:])

            # Transparent layer for the text.
            txt_layer = Image.new("RGBA", img.size, (255, 255, 255, 0))
            draw = ImageDraw.Draw(txt_layer)

            left, top, right, bottom = draw.multiline_textbbox((0, 0), text_to_draw, font=font, spacing=12)
            text_w, text_h = right - left, bottom - top

            # Text position.
            t_x, t_y = get_coords(args.t_pos, img_w, img_h, text_w, text_h, args.t_x, args.t_y)

            # Text opacity (0-255).
            t_alpha = int((args.t_opacity / 100.0) * 255)

            draw.multiline_text((t_x, t_y), text_to_draw, fill=(255, 255, 255, t_alpha), font=font, spacing=12, align=args.t_align.lower())

            # Composite the text layer onto the photo.
            img = Image.alpha_composite(img, txt_layer)

            # Save as JPEG.
            out_img = img.convert("RGB")
            out_img.save(os.path.join(args.output, f"{file_id}.jpg"), quality=100)

            print(f"[SUCCESS] [{file_id}] Successfully stamped and saved.", flush=True)

        except Exception as e:
            print(f"[ERROR] [{file_id}] Processing failed: {str(e)}", flush=True)

        # Report progress (0-100%) to the C frontend.
        percent = int((idx / total_blocks) * 100)
        print(f"PROGRESS:{percent}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    # Path arguments.
    parser.add_argument("--docx", required=True)
    parser.add_argument("--photos", required=True)
    parser.add_argument("--compass", required=True)
    parser.add_argument("--output", required=True)

    # Compass settings.
    parser.add_argument("--c-size", type=int, default=140)
    parser.add_argument("--c-pos", type=str, default="UpLeft")
    parser.add_argument("--c-opacity", type=int, default=76)
    parser.add_argument("--c-x", type=int, default=76)
    parser.add_argument("--c-y", type=int, default=76)

    # Text settings.
    parser.add_argument("--t-size", type=int, default=19)
    parser.add_argument("--t-pos", type=str, default="UpLeft")
    parser.add_argument("--t-opacity", type=int, default=76)
    parser.add_argument("--t-x", type=int, default=76)
    parser.add_argument("--t-y", type=int, default=76)
    parser.add_argument("--t-font", type=str, default="Arial")
    parser.add_argument("--t-align", type=str, default="left")

    args = parser.parse_args()

    stamp_field_photos(args)
    print("[INFO] All tasks completed successfully. Wonderhoy!", flush=True)
