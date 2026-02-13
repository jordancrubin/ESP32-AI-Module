#pragma once
#include <Arduino.h>

// Replace the array below with your JPG hex data.
// Use a tool like http://tomeko.net/online_tools/file_to_hex.php to convert your .jpg file.

const uint8_t boot_logo[] PROGMEM = {
  // Placeholder: A tiny valid JPG header (Start of Image)
  // PASTE YOUR HEX DATA HERE
  0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10
};