import sys
import os

def convert_jpg(image_path):
    with open(image_path, "rb") as f:
        data = f.read()
    
    print("#pragma once")
    print("#include <Arduino.h>")
    print("")
    print("// JPEG Data")
    print("const uint8_t boot_logo[] PROGMEM = {")
    
    hex_data = ", ".join(f"0x{b:02x}" for b in data)
    print(hex_data)
    print("};")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 jpg2header.py <image.jpg>")
        sys.exit(1)
    convert_jpg(sys.argv[1])