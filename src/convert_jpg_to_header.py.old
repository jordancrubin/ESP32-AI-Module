import sys
from PIL import Image

def convert_image(image_path):
    img = Image.open(image_path).convert('RGB')
    width, height = img.size
    
    print("#pragma once")
    print("#include <Arduino.h>")
    print("")
    print(f"const int boot_logo_w = {width};")
    print(f"const int boot_logo_h = {height};")
    print("")
    print("// Raw RGB565 Pixel Data")
    print("const uint16_t boot_logo[] PROGMEM = {")
    
    data = []
    for y in range(height):
        for x in range(width):
            r, g, b = img.getpixel((x, y))
            # Convert RGB888 to RGB565
            rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            data.append(f"0x{rgb565:04X}")
    
    print(", ".join(data))
    print("};")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 convert_jpg_to_header.py <image_file>")
        sys.exit(1)
    convert_image(sys.argv[1])