import os
import sys
import argparse

sys.path.append(os.path.dirname(os.path.abspath(__file__)))
import stamp_engine

def main():
    parser = argparse.ArgumentParser(description="Bridge GUI C to Engine Stampy")

    # Path Arguments dari GUI
    parser.add_argument("--docx", required=False, help="Path file docx/xlsx metadata")
    parser.add_argument("--photos", required=True)
    parser.add_argument("--compass", required=True)
    parser.add_argument("--output", required=True)

    # Compass Settings dari GUI
    parser.add_argument("--c-size", type=int, default=140)
    parser.add_argument("--c-pos", type=str, default="UpLeft")
    parser.add_argument("--c-opacity", type=int, default=76)
    parser.add_argument("--c-x", type=int, default=76)
    parser.add_argument("--c-y", type=int, default=76)

    # Text Settings dari GUI
    parser.add_argument("--t-size", type=int, default=19)
    parser.add_argument("--t-pos", type=str, default="UpLeft")
    parser.add_argument("--t-opacity", type=int, default=76)
    parser.add_argument("--t-x", type=int, default=76)
    parser.add_argument("--t-y", type=int, default=76)
    parser.add_argument("--t-font", type=str, default="Arial")
    parser.add_argument("--t-align", type=str, default="left")

    args = parser.parse_args()

    try:
        stamp_engine.stamp_field_photos(args)
    except Exception as e:
        print(f"[ERROR] Bridge execution failed: {str(e)}", flush=True)

if __name__ == "__main__":
    main()
