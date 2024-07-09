#include <Metal/Metal.h>

#include <radray/logger.h>

extern "C" void RadrayPrintMTLFunctionLog(id<MTLLogContainer> logs) {
  if (logs) {
    for (id<MTLFunctionLog> log in logs) {
      RADRAY_INFO_LOG("[MTLFunctionLog]: {}", log.debugDescription.UTF8String);
    }
  }
}
