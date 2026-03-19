/*
  CommandProcessor.h - ESP32 AI Module
  Handles local voice command interception.
*/
#pragma once
#include <Arduino.h>

struct CommandResult {
    bool handled;
    String response;
    bool runAecTest;
    bool systemReboot;
    bool showBSOD;
    bool showGuruMeditation;
};

class CommandProcessor {
public:
    static CommandResult processCommand(const String& text);
};