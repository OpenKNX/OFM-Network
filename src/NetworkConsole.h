#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#pragma once
#include "OpenKNX.h"
#include <string>

class NetworkModule;

// Console command handling for the NetworkModule.
// Extracted from NetworkModule so the module class stays focused on the network
// stack while all console parsing and help output live here.
//
// The module's entry in the *general* console help stays compact (a single line
// pointing at 'net ?'), while the full, detailed command list is printed by the
// 'net ?' sub-help implemented here.
class NetworkConsole
{
  public:
    explicit NetworkConsole(NetworkModule &module) : _module(module) {}

    // Parse and execute a console command. Returns true if the command was handled.
    bool processCommand(const std::string cmd, bool debugKo);

    // Print the detailed network sub-help (shown via 'net ?').
    void showSubHelp();

  private:
    NetworkModule &_module;

    // Provide the same log prefix ("Network") as the module, so the logInfoP/logErrorP
    // macros (which call logPrefix()) produce output identical to the previous location.
    const std::string logPrefix();
};
#endif
