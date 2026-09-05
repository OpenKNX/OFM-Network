#pragma once
// Manages the ETH link mode on top of the raw platform link drivers in this folder:
//   RP2040 -> W5500Phy   (raw W5500 PHYCFGR over SPI)
//   ESP32  -> Esp32EthLink (esp_eth ioctl, stop/reconfigure/start)
//
// It owns the fixed-speed override and the OPENKNX_ETH_AUTO_FALLBACK ladder. The mode comes from the ETS
// parameter "LAN-Modus", else the build flag OPENKNX_ETH_FORCE_SPEED, else auto-negotiation; the console
// `net eth` is a live-only override that is deliberately NOT persisted (a reboot returns to ETS). NetworkModule holds one instance, forwards its OpenKNX::Module hooks
// (flash / startup-delay) and the ~2 Hz link tick here, and exposes it to NetworkConsole via ethLink().
// Only compiled for W5500 LAN targets.
#if defined(KNX_IP_LAN) && (defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32))
#include "OpenKNX.h"
#if defined(ARDUINO_ARCH_RP2040)
#include "W5500Phy.h"
#endif

namespace OpenKNX { namespace Network { class Module; } } // real type; NetworkModule is a using-alias of this

class EthLinkManager
{
  public:
    EthLinkManager(OpenKNX::Network::Module &module) : _module(module) {}

    // Console `net eth`: show current mode (no arg) / set one live (auto|10|100, half|full), until reboot.
    void logStatus();
    bool setMode(uint8_t mode, bool full); // mode: 0=auto, 1=10 Mbit, 2=100 Mbit
    void logLinkInfo();                    // one-line live link status for 'net' (X Mbit Y-duplex (fixed/auto))

#ifdef OPENKNX_ETH_AUTO_FALLBACK
    void tick(bool carrierUp, bool established); // ~2 Hz link tick; walks the ladder (no-op if a fixed mode is set)
    void resetLadder();                          // restart the auto-fallback search from auto-negotiation
#endif

#if defined(ARDUINO_ARCH_ESP32)
    void applyBeforeBegin(); // apply the ETS / build-flag fixed mode BEFORE ETH.begin()
#endif

#if defined(ARDUINO_ARCH_RP2040)
    void resetLink();      // `net reset`: bounce the W5500 PHY (like a cable re-plug), no reboot
    // Raw VERSIONR read (0x04 = a present W5500). Used by NetworkModule's W5500 self-heal to classify a
    // failed begin(): 0x04 => chip present, only socket OPEN not ready (transient); else => no SPI comms.
    uint8_t chipVersion() { return _phy.chipVersion(); }
    // Raw PHY access for the console diagnostics ('net phy'): version at a chosen clock, PHYCFGR, link.
    W5500Phy &phy() { return _phy; }
    // Apply the ETS "LAN-Modus" live via the PHY. Called after every successful W5500 bring-up (boot,
    // self-heal) and once more from the startup-delay hook; a no-op while ETS says "Automatisch".
    void applyEtsMode();
    void onStartupDelay(); // Module hook: startup delay over -> apply the ETS mode
    uint16_t flashSize();  // OpenKNX module-flash slot (reserved, nothing persisted any more)
    void writeFlash();
    void readFlash(const uint8_t *data, uint16_t size);
#endif

  private:
    const std::string logPrefix(); // keeps log output identical to the module ("Network")

#ifdef OPENKNX_ETH_AUTO_FALLBACK
    void applyStage(uint8_t stage); // apply a ladder stage LIVE + start settle/deadline windows
#endif
#if defined(ARDUINO_ARCH_RP2040)
    void applyFixedLive(); // apply the loaded fixed mode live via W5500Phy (boot / reset)
#endif

    OpenKNX::Network::Module &_module;
    uint8_t _mode = 0xFF; // 0xFF=unset(auto), 0=auto, 1=10, 2=100
    bool _full = false;   // half-duplex default when forced

#if defined(ARDUINO_ARCH_RP2040)
    W5500Phy _phy{ETH_SPI_INTERFACE, PIN_ETH_SS};
#endif

#ifdef OPENKNX_ETH_AUTO_FALLBACK
    // Ladder tuning.
    static constexpr uint8_t ETH_FLAP_THRESHOLD = 4;        // this many carrier flaps -> escalate a stage
    static constexpr uint32_t ETH_STABLE_MS = 5000;         // ESTABLISHED (link+IP) this long -> lock the stage
    static constexpr uint32_t ETH_SETTLE_MS = 4000;         // ignore flaps this long after we changed the mode
    static constexpr uint32_t ETH_STAGE_TIMEOUT_MS = 15000; // carrier up but no IP this long -> escalate (dead link)
    static constexpr uint32_t ETH_UNLOCK_MS = 4000;         // established lost this long on a locked link -> restart
    uint8_t _stage = 0;                                     // 0=autoneg,1=100F,2=100H,3=10F,4=10H
    bool _locked = false;                                   // converged to a stable (established) stage
    bool _prevUp = false;                                   // previous carrier state (for flap edge detection)
    uint8_t _flapCount = 0;                                 // consecutive carrier flaps at the current stage
    uint32_t _lastStableStart = 0;                          // when the current ESTABLISHED (IP) link started (0 = not established)
    uint32_t _settleUntil = 0;                              // ignore flaps until this time (just after a mode change)
    uint32_t _stageDeadline = 0;                            // carrier-up-but-no-IP escalation deadline (0 = arm on first tick)
    uint32_t _lostSince = 0;                                // when a LOCKED link first lost established (0 = not lost); debounces unlock
    uint8_t _wrapCount = 0;                                 // fruitless full ladder passes (no IP) -> park after the cap
    bool _resting = false;                                  // parked at the safe fallback speed, waiting for a real cable (quiet)
    static constexpr uint8_t ETH_MAX_WRAPS = 1;             // after this many fruitless full passes -> rest at 10 Mbit half
#endif
};
#endif
