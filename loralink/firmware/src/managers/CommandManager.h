#ifndef COMMAND_MANAGER_H
#define COMMAND_MANAGER_H

#include "../config.h"
#include <Arduino.h>
#include <functional>
#include <map>

typedef std::function<void(const String &, CommInterface)> CommandHandler;

class CommandManager {
public:
  static CommandManager &getInstance() {
    static CommandManager instance;
    return instance;
  }

  // Main entry point for ANY text command from ANY interface
  void handleCommand(const String &fullCmd, CommInterface source);
  void handleBinaryCommand(const uint8_t *data, size_t len,
                           CommInterface source);
  void executeLocalCommand(const String &subCmd, CommInterface source);
  void restoreHardwareState();

  void registerCommand(const String &cmd, CommandHandler handler);

  int getPinFromName(const String &name);

  // Helper to get interface name string
  static const char *interfaceName(CommInterface ifc);

  // Guarded deep-sleep entry: checks mains power, PC attachment, broadcasts
  // countdown, logs PC alert if no cancel received. Both the SLEEP command and
  // the battery monitor callback route through here.
  static void executeSleep(float hours, const String &trigger);

  // Route a response string back to the originating interface
  void sendResponse(const String &msg, CommInterface source);

private:
  CommandManager();
  void initRegistry();
  std::map<String, CommandHandler> _commandRegistry;
};

#endif // COMMAND_MANAGER_H
