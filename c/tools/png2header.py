#!/usr/bin/env python3
"""
Convert PNG image to C header for embedding in TinyOS.
Outputs raw BGRA pixel data (matching framebuffer format).

Usage: python3 png2header.py input.png output.h [max_width] [max_height]
"""

import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("Error: Pillow not installed. Run: pip3 install Pillow")
    sys.exit(1)

def convert_png_to_header(input_path, output_path, max_width=360, max_height=640):
    # Load image
    img = Image.open(input_path)

    # Convert to RGBA if needed
    if img.mode != 'RGBA':
        img = img.convert('RGBA')

    # Resize if too large (maintain aspect ratio)
    if img.width > max_width or img.height > max_height:
        img.thumbnail((max_width, max_height), Image.Resampling.LANCZOS)

    width, height = img.size
    pixels = list(img.getdata())

    # Generate variable name from filename
    name = Path(input_path).stem.replace('-', '_').replace('.', '_').upper()

    # Generate C header
    header = f"""/*
 * Auto-generated image data from {Path(input_path).name}
 * Size: {width}x{height}
 */

#ifndef IMAGE_{name}_H
#define IMAGE_{name}_H

#include "types.h"

#define {name}_WIDTH  {width}
#define {name}_HEIGHT {height}

/* BGRA pixel data (matches framebuffer format) */
static const uint32_t {name.lower()}_data[{width * height}] = {{
"""

    # Convert pixels to BGRA format
    pixel_lines = []
    line = []
    for i, (r, g, b, a) in enumerate(pixels):
        # BGRA format: 0xAARRGGBB but framebuffer uses 0x00RRGGBB
        # Actually the framebuffer is BGRX where X is ignored
        # So we want: (B << 0) | (G << 8) | (R << 16)
        bgra = (r << 16) | (g << 8) | b
        line.append(f"0x{bgra:08X}")

        if len(line) == 8:
            pixel_lines.append(", ".join(line))
            line = []

    # Add remaining pixels
    if line:
        pixel_lines.append(", ".join(line))

    header += "    " + ",\n    ".join(pixel_lines)
    header += f"""
}};

#endif /* IMAGE_{name}_H */
"""

    # Write output
    with open(output_path, 'w') as f:
        f.write(header)

    print(f"Converted {input_path} -> {output_path}")
    print(f"  Size: {width}x{height}")
    print(f"  Pixels: {width * height}")
    print(f"  Data size: {width * height * 4} bytes")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 png2header.py input.png output.h [max_width] [max_height]")
        sys.exit(1)

    input_path = sys.argv[1]
    output_path = sys.argv[2]
    max_width = int(sys.argv[3]) if len(sys.argv) > 3 else 360
    max_height = int(sys.argv[4]) if len(sys.argv) > 4 else 640

    convert_png_to_header(input_path, output_path, max_width, max_height)
