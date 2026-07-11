#include "EthLinkManager.h"
#if defined(KNX_IP_LAN) && (defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32))
#include "NetworkModule.h" // for _module.name() (log prefix)
#if defined(ARDUINO_ARCH_ESP32)
#include "Esp32EthLink.h"
#include <ETH.h>
#include <Preferences.h>
#endif

// Keep the same prefix as the module ("Network") so console/log output is unchanged after the extraction.
const std::string EthLinkManager::logPrefix()
{
    return _module.name();
}

// ---- console 'net eth' -----------------------------------------------------------------------------

// 'net eth' with no argument: show the current link mode.
void EthLinkManager::logStatus()
{
#if defined(ARDUINO_ARCH_ESP32)
    uint8_t m = 0xFF;
    bool f = false;
    Preferences p;
    if (p.begin("ethlink", true))
    {
        m = p.getUChar("mode", 0xFF);
        f = p.getBool("full", false);
        p.end();
    }
    const char *s = (m == 0xFF) ? "auto (default, no override)"
                    : (m == 0)  ? "auto-negotiation"
                    : (m == 1)  ? (f ? "10 Mbit full-duplex" : "10 Mbit half-duplex")
                                : (f ? "100 Mbit full-duplex" : "100 Mbit half-duplex");
    logInfoP("ETH link: %s", s);
#elif defined(ARDUINO_ARCH_RP2040)
    W5500Phy::Status st = _phy.read();
    logInfoP("ETH link: %s, %s Mbit %s-duplex", st.linked ? "up" : "down",
             st.speed100 ? "100" : "10", st.fullDuplex ? "full" : "half");
    const char *cfg = (_mode == 0xFF || _mode == 0) ? "auto-negotiation"
                      : (_mode == 1)                ? (_full ? "10 Mbit full-duplex" : "10 Mbit half-duplex")
                                                    : (_full ? "100 Mbit full-duplex" : "100 Mbit half-duplex");
    logInfoP("Configured: %s", cfg);
#endif
    logInfoP("Usage: net eth <auto|10|100> [half|full]  (applied live, persisted)");
}

// Live one-line link status for 'net' / showNetworkInformations: "ETH link: X Mbit Y-duplex (fixed/auto)".
void EthLinkManager::logLinkInfo()
{
#if defined(ARDUINO_ARCH_ESP32)
    logInfoP("ETH link: %u Mbit %s-duplex (%s)", (unsigned)ETH.linkSpeed(),
             ETH.fullDuplex() ? "full" : "half", ETH.autoNegotiation() ? "autoneg" : "fixed");
#elif defined(ARDUINO_ARCH_RP2040)
    bool fixed = (_mode == 1 || _mode == 2);
#ifdef OPENKNX_ETH_AUTO_FALLBACK
    fixed = fixed || (_stage > 0); // the auto-fallback ladder forced a fixed speed
#endif
    logInfoP("ETH link: %u Mbit %s-duplex (%s)", (unsigned)_phy.linkSpeed(),
             _phy.fullDuplex() ? "full" : "half", fixed ? "fixed" : "auto-negotiation");
#endif
}

// 'net eth <auto|10|100> [half|full]': set the mode, persist it, and apply it LIVE (no reboot).
bool EthLinkManager::setMode(uint8_t mode, bool full)
{
    _mode = mode;
    _full = full;
    const bool speed100 = (mode == 2);
    bool ok = true;

#if defined(ARDUINO_ARCH_ESP32)
    Preferences p; // persist for the next boot (applied before begin() via applyBeforeBegin)
    if (p.begin("ethlink", false))
    {
        p.putUChar("mode", mode);
        p.putBool("full", full);
        p.end();
    }
#ifdef OPENKNX_ETH_AUTO_FALLBACK
    if (mode == 0) resetLadder(); // 'auto' -> fresh search; fixed -> ladder gated off via _mode
#endif
    ok = (mode == 0) ? Esp32EthLink::setAutoNeg() : Esp32EthLink::forceMode(speed100, full);
#elif defined(ARDUINO_ARCH_RP2040)
#ifdef OPENKNX_ETH_AUTO_FALLBACK
    if (mode == 0) resetLadder();
#endif
    if (mode == 0) _phy.setAutoNeg();
    else
        _phy.forceMode(speed100, full);
    openknx.flash.save(); // persist the manual fixed mode (writeFlash)
#endif

    if (mode == 0) logInfoP("ETH link -> auto-negotiation (applied live%s)", ok ? "" : " - FAILED");
    else
        logInfoP("ETH link -> %s Mbit %s-duplex (applied live%s)", mode == 1 ? "10" : "100", full ? "full" : "half", ok ? "" : " - FAILED");
    return ok;
}

// ---- fixed-mode apply ------------------------------------------------------------------------------

#if defined(ARDUINO_ARCH_ESP32)
// Apply the effective ETH link mode BEFORE begin(): runtime override from NVS (console `net eth ...`)
// wins, else the compile-time OPENKNX_ETH_FORCE_SPEED flag, else auto-negotiation.
void EthLinkManager::applyBeforeBegin()
{
    uint8_t mode = 0xFF; // 0xFF = no NVS override
    bool full = false;
    Preferences prefs;
    if (prefs.begin("ethlink", true))
    {
        mode = prefs.getUChar("mode", 0xFF);
        full = prefs.getBool("full", false);
        prefs.end();
    }

    bool forced = false;
    if (mode != 0xFF)
    {
        forced = (mode != 0); // 0 = explicit auto override
    }
#if defined(OPENKNX_ETH_FORCE_SPEED)
    else
    {
        forced = true;
        mode = (OPENKNX_ETH_FORCE_SPEED >= 100) ? 2 : 1;
#ifdef OPENKNX_ETH_FORCE_FULLDUPLEX
        full = true;
#endif
    }
#endif

    if (forced)
    {
        const int mbit = (mode == 2) ? 100 : 10;
        logInfoP("ETH: fixed link %d Mbit %s-duplex (autoneg off)", mbit, full ? "full" : "half");
        ETH.setAutoNegotiation(false);
        ETH.setLinkSpeed(mbit);
        ETH.setFullDuplex(full);
    }
    else if (mode == 0)
    {
        logInfoP("ETH: auto-negotiation (console override)");
        ETH.setAutoNegotiation(true);
    }
    // else: no override and no build flag -> leave driver default (auto-negotiation)

    _mode = mode;
    _full = full;
#ifdef OPENKNX_ETH_AUTO_FALLBACK
    if (_mode == 1 || _mode == 2)
        logDebugP("ETH forced to fixed %d Mbit %s-duplex -> auto-fallback ladder DISABLED (use 'net eth auto' to re-enable)",
                  _mode == 2 ? 100 : 10, _full ? "full" : "half");
    else
        logDebugP("ETH auto-fallback ladder active (auto-negotiation)");
#endif
}
#endif // ARDUINO_ARCH_ESP32

#if defined(ARDUINO_ARCH_RP2040)
// Apply the loaded fixed ETH link mode LIVE via the W5500 PHYCFGR (no reboot needed).
void EthLinkManager::applyFixedLive()
{
    if (_mode == 1)
    {
        logInfoP("ETH: fixed link 10 Mbit %s-duplex", _full ? "full" : "half");
        _phy.forceMode(false, _full);
    }
    else if (_mode == 2)
    {
        logInfoP("ETH: fixed link 100 Mbit %s-duplex", _full ? "full" : "half");
        _phy.forceMode(true, _full);
    }
    else if (_mode == 0)
    {
        logInfoP("ETH: auto-negotiation");
        _phy.setAutoNeg();
    }
    // _mode == 0xFF -> leave driver default (auto)
}
#endif // ARDUINO_ARCH_RP2040

// ---- auto-fallback ladder --------------------------------------------------------------------------

#ifdef OPENKNX_ETH_AUTO_FALLBACK
// Human-readable name of a link mode for the log (stage 0 = auto-negotiation, 1..4 = fixed speeds).
static const char *ethStageName(uint8_t stage)
{
    switch (stage)
    {
        case 1: return "100 Mbit full-duplex";
        case 2: return "100 Mbit half-duplex";
        case 3: return "10 Mbit full-duplex";
        case 4: return "10 Mbit half-duplex";
        default: return "auto-negotiation";
    }
}

// Apply a ladder stage LIVE (0 = auto-negotiation, 1..4 = fixed speed) and start the settle/deadline windows.
void EthLinkManager::applyStage(uint8_t stage)
{
    _stage = stage;
    // stage -> (speed100, fullDuplex); stage 0 = auto-negotiation. 1=100F 2=100H 3=10F 4=10H.
    const bool speed100 = (stage == 1 || stage == 2);
    const bool full = (stage == 1 || stage == 3);
#if defined(ARDUINO_ARCH_RP2040)
    if (stage == 0) _phy.setAutoNeg();
    else
        _phy.forceMode(speed100, full);
#elif defined(ARDUINO_ARCH_ESP32)
    const bool applied = (stage == 0) ? Esp32EthLink::setAutoNeg() : Esp32EthLink::forceMode(speed100, full);
    if (!applied) logErrorP("ETH: could not apply link stage (esp_eth stop/reconfigure/start rejected)");
#endif
    if (stage == 0)
        logDebugP("Link speed set to auto-negotiation");
    else if (stage == 1)
        logWarningP("Warning: Auto-negotiation unstable, trying fixed %s", ethStageName(stage));
    else
        logWarningP("Warning: Link still unstable, trying fixed %s", ethStageName(stage));
    const uint32_t nowMs = millis();
    _settleUntil = nowMs + ETH_SETTLE_MS;
    _stageDeadline = nowMs + ETH_STAGE_TIMEOUT_MS; // this stage must deliver an IP within this window
    _lastStableStart = 0;
    _flapCount = 0;
}

// Reset the ladder to a fresh auto-negotiation search (does NOT apply a mode - the caller applies it live).
// Shared by console 'net eth auto' and resetLink()/resetNetwork() so the reset cannot drift between paths.
void EthLinkManager::resetLadder()
{
    const uint32_t nowMs = millis();
    _stage = 0;
    _locked = false;
    _flapCount = 0;
    _lastStableStart = 0;
    _lostSince = 0;
    _wrapCount = 0;
    _resting = false;
    _settleUntil = nowMs + ETH_SETTLE_MS;
    _stageDeadline = nowMs + ETH_STAGE_TIMEOUT_MS;
}

// Called from NetworkModule::checkLinkStatus (~2 Hz) with (carrierUp, established). Walks the ladder until a
// stage delivers a real ESTABLISHED connection (link + IP), then locks it. Carrier alone does NOT count - that
// is what rejects a duplex-mismatched fixed speed: link LED on, but no IP ever arrives. A stage escalates on
// carrier flapping OR when the carrier stays up but no IP shows up within ETH_STAGE_TIMEOUT_MS. Inactive once
// a fixed mode is set.
void EthLinkManager::tick(bool carrierUp, bool established)
{
    if (_mode == 1 || _mode == 2) return; // user forced a fixed mode -> no fallback

    uint32_t now = millis();
    if (_stageDeadline == 0) _stageDeadline = now + ETH_STAGE_TIMEOUT_MS; // arm on the first tick (initial autoneg)

    // Locked to a stable, established stage: a real disconnect (cable pulled / switch changed) restarts the
    // search. Debounced by ETH_UNLOCK_MS so a single transient loss of the IP (a brief carrier glitch on an
    // otherwise good link) does NOT tear down a converged fixed speed.
    if (_locked)
    {
        if (established)
            _lostSince = 0;
        else if (_lostSince == 0)
            _lostSince = now;
        else if ((now - _lostSince) >= ETH_UNLOCK_MS)
        {
            logWarningP("Connection lost, restarting from auto-negotiation");
            _locked = false;
            _lostSince = 0;
            applyStage(0);
        }
        _prevUp = carrierUp;
        return;
    }

    if ((int32_t)(now - _settleUntil) < 0)
    {
        _prevUp = carrierUp;
        return;
    } // settling after a mode change

    if (established)
    {
        // A real IP link is present -> drop any "no cable" rest state so the ladder fully re-searches.
        _wrapCount = 0;
        _resting = false;
        // Real IP connectivity - the only thing that makes a stage "good". A carrier-only stage never gets here.
        if (_lastStableStart == 0) _lastStableStart = now;
        else if ((now - _lastStableStart) >= ETH_STABLE_MS)
        {
            if (_stage >= 1)
            {
                _locked = true;
                _lostSince = 0;
                logInfoP("Link stable at fixed %s, keeping this setting", ethStageName(_stage));
                // Not persisted: after a reboot the ladder always re-searches from auto-negotiation.
            }
            else
            {
                _flapCount = 0;                              // auto-negotiation works -> keep it and
                _stageDeadline = now + ETH_STAGE_TIMEOUT_MS; // never time out while it stays established
            }
        }
    }
    else // no working (IP) connection at this stage yet
    {
        _lastStableStart = 0;
        // Carrier just came up (cable plugged / link back): give this stage a fresh IP window instead of a
        // stale _stageDeadline (from boot/no-cable) that would escalate the instant the link returns.
        if (carrierUp && !_prevUp) _stageDeadline = now + ETH_STAGE_TIMEOUT_MS;
        bool escalate = false;
        // While resting (a full ladder pass found no IP -> almost certainly no cable) we stop reacting to the
        // W5500's spurious carrier flaps, so a cable-less port stays quiet instead of cycling 0->4 forever.
        if (!_resting && !carrierUp && _prevUp) // carrier down edge = one flap
        {
            ++_flapCount;
            logDebugP("ETH link unstable (%u/%u) at %s", _flapCount, ETH_FLAP_THRESHOLD, ethStageName(_stage));
            if (_flapCount >= ETH_FLAP_THRESHOLD) escalate = true;
        }
        else if (!_resting && carrierUp && (int32_t)(now - _stageDeadline) >= 0) // carrier up but no IP in time
        {
            logWarningP("%s: carrier up but no IP -> trying next speed", ethStageName(_stage));
            escalate = true;
        }
        if (escalate)
        {
            if (_stage < 4)
                applyStage(_stage + 1); // escalate live to the next stage
            else if (++_wrapCount < ETH_MAX_WRAPS)
            {
                // Nothing worked this pass -> wrap to auto-negotiation and keep trying (never silently offline).
                logWarningP("No working link speed found this cycle, retrying from auto-negotiation");
                applyStage(0);
            }
            else
            {
                // A full pass tried every speed with no IP -> almost certainly NO CABLE. Park at the safest
                // fixed speed (10 Mbit half) and go quiet; a real cable re-asserts carrier + gets an IP, hits
                // the established branch above, clears _resting and re-searches. Ends the idle log churn.
                if (!_resting)
                {
                    applyStage(4); // 10 Mbit half-duplex = known-safe rest speed (autoneg is what EEE flaps on)
                    _resting = true;
                    logInfoP("No link found (no cable?) -> resting at %s, waiting for cable", ethStageName(4));
                }
            }
        }
    }
    // While parked, keep pushing the escalation deadline so the guarded triggers above never re-fire (quiet idle).
    if (_resting) _stageDeadline = now + ETH_STAGE_TIMEOUT_MS;
    _prevUp = carrierUp;
}
#endif // OPENKNX_ETH_AUTO_FALLBACK

// ---- RP2040: 'net reset' + OpenKNX module-flash persistence ----------------------------------------

#if defined(ARDUINO_ARCH_RP2040)
// `net reset`: re-initialise the W5500 link WITHOUT rebooting - like unplugging and re-plugging the cable.
// (A driver end()->begin() would brick the W5500 in a fatalError loop; a full reboot is 'r'.) Bounce the PHY
// so the stack re-runs DHCP / re-binds KNX; in auto mode the link-speed search restarts from auto-negotiation.
void EthLinkManager::resetLink()
{
    logInfoP("Re-initialising W5500 link (like cable re-plug)");
#ifdef OPENKNX_ETH_AUTO_FALLBACK
    _locked = false;
    if (_mode == 1 || _mode == 2) applyFixedLive(); // forced fixed mode -> re-apply (bounces the PHY)
    else
        applyStage(0); // auto -> restart the search from auto-negotiation
#else
    if (_mode == 1 || _mode == 2) applyFixedLive();
    else
        _phy.setAutoNeg(); // plain PHY reset / auto-negotiation
#endif
}

// Module hook: module flash has loaded -> re-apply a persisted MANUAL fixed mode (the auto-fallback stage is
// NOT persisted; auto mode just re-searches from auto-negotiation).
void EthLinkManager::onStartupDelay()
{
    if (_mode == 1 || _mode == 2)
    {
        applyFixedLive();
        return;
    } // manual fixed mode persists across reboot
#ifdef OPENKNX_ETH_AUTO_FALLBACK
    _settleUntil = millis() + ETH_SETTLE_MS; // give the initial auto-negotiation a moment before counting
#endif
}

uint16_t EthLinkManager::flashSize()
{
    return 3; // [version][mode][fullDuplex] - the auto-fallback stage is intentionally NOT persisted
}

void EthLinkManager::writeFlash()
{
    openknx.flash.writeByte(2); // format version
    openknx.flash.writeByte(_mode);
    openknx.flash.writeByte(_full ? 1 : 0);
}

void EthLinkManager::readFlash(const uint8_t *data, uint16_t size)
{
    if (size < 3 || data[0] != 2) return; // unknown/old version -> keep defaults
    _mode = data[1];                      // only the manual fixed mode persists across a reboot;
    _full = (data[2] != 0);               // the auto-fallback stage is NOT restored (ladder re-searches from autoneg)
}
#endif // ARDUINO_ARCH_RP2040
#endif
