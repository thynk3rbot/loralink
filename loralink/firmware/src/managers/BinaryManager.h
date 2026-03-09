#ifndef BINARY_MANAGER_H
#define BINARY_MANAGER_H

#include "../config.h"
#include <Arduino.h>

class BinaryManager {
public:
  static BinaryManager &getInstance() {
    static BinaryManager instance;
    return instance;
  }

  // Parses a raw binary payload.
  // returns true if handled as binary.
  bool handleBinary(const uint8_t *data, size_t len, CommInterface source);

  // Creates a binary frame for a common command
  size_t createBinaryFrame(uint8_t target, BinaryCmd cmd, uint8_t *args,
                           size_t argLen, uint8_t *outBuf);

private:
  BinaryManager() {}
  uint8_t calculateSum(const uint8_t *data, size_t len);
};

#endif
