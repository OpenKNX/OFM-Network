#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#include "NetworkConsole.h"
#include "NetworkModule.h"

// Same prefix as the module ("Network") so log output is unchanged after the move.
const std::string NetworkConsole::logPrefix()
{
    return _module.name();
}

bool NetworkConsole::processCommand(const std::string cmd, bool debugKo)
{
    if (debugKo) return false;

    // 'net ?' / 'n ?' -> detailed sub-help for the network module (net and n are aliases).
    if (cmd == "net ?" || cmd == "n ?")
    {
        showSubHelp();
        return true;
    }

    if (!debugKo && (cmd == "n" || cmd == "net"))
    {
        _module.showNetworkInformations(true);
        return true;
    }

#if MASK_VERSION == 0x091A || MASK_VERSION == 0x57B0
    else if (cmd == "net mc")
    {
        IPAddress address = _module.GetIpProperty(PID_ROUTING_MULTICAST_ADDRESS);
        openknx.logger.logWithPrefixAndValues("Network", "Multicast address is set to %s", address.toString().c_str());
        return true;
    }

    else if (cmd.compare(0, 7, "net mc ") == 0)
    {
        IPAddress new_address;
        std::string new_address_str = cmd.length() >= 7 ? cmd.substr(7) : "reset";

        if (new_address_str == "reset") new_address_str = "0.0.0.0";

        new_address.fromString(new_address_str.c_str());

        _module.setMulticastAddress(new_address, true);
        return true;
    }
#endif

    else if (cmd == "net reset")
    {
        _module.resetNetwork();
        return true;
    }

#ifdef KNX_IP_LAN
    // 'net eth' / 'net eth <auto|10|100> [half|full]' - all platform specifics live in EthLinkManager.
    else if (cmd == "net eth" || cmd.compare(0, 8, "net eth ") == 0)
    {
        std::string arg = (cmd.length() > 7) ? cmd.substr(7) : "";
        while (!arg.empty() && arg[0] == ' ')
            arg.erase(0, 1); // trim leading spaces

        if (arg.empty())
        {
            _module._ethLink.logStatus();
            return true;
        } // no arg -> show current mode

        std::string speed = arg, dup;
        size_t sp = arg.find(' ');
        if (sp != std::string::npos)
        {
            speed = arg.substr(0, sp);
            dup = arg.substr(sp + 1);
            while (!dup.empty() && dup[0] == ' ')
                dup.erase(0, 1);
        }

        uint8_t mode;
        if (speed == "auto") mode = 0;
        else if (speed == "10")
            mode = 1;
        else if (speed == "100")
            mode = 2;
        else
        {
            logErrorP("Usage: net eth <auto|10|100> [half|full]");
            return true;
        }

        bool full = false;
        if (mode != 0)
        {
            if (dup == "full") full = true;
            else if (dup.empty() || dup == "half")
                full = false;
            else
            {
                logErrorP("Duplex must be 'half' or 'full'");
                return true;
            }
        }

        _module._ethLink.setMode(mode, full); // set + persist + apply live (per-platform, inside the manager)
        return true;
    }
#endif

#ifdef KNX_IP_WIFI
    else if (cmd == "erase wifi")
    {
        _module.saveWifiSettings("", "", true); // triggers the restart timer
        logInfoP("Wifi settings erased");
        return true;
    }
    else if (cmd.compare(0, 4, "wifi") == 0 && cmd.length() > 6)
    {
        // Command: "wifi<SEP><ssid><SEP><psk>" with <SEP>=" " compatible to previous command
        char delimiter = cmd[4];
        std::string ssid_psk = cmd.substr(5);
        size_t pos = ssid_psk.find(delimiter);

        if (pos != std::string::npos)
        {
            std::string ssid = ssid_psk.substr(0, pos);
            std::string psk = ssid_psk.substr(pos + 1);

            if (ssid.empty())
                logInfoP("Wifi settings erased");
            else
                logInfoP("WLAN SSID: %s", ssid.c_str());
            _module.saveWifiSettings(ssid.c_str(), psk.c_str(), true); // triggers the restart timer

            return true;
        }
        else
        {
            logErrorP("Expected format: wifi<Sep><SSID><Sep><PSK> with <Sep> single char not in <SSID>");
            return false;
        }
    }
#endif

    return false;
}

// Detailed network command help, shown via 'net ?' / 'n ?'. Mirrors the build guards of the individual
// commands so only the ones actually available are listed. Command column is capped at 23 chars
// (OPENKNX_MAX_LOG_PREFIX_LENGTH) - keep each command string within that so nothing is truncated.
void NetworkConsole::showSubHelp()
{
    openknx.logger.begin();
    openknx.logger.log("");
    openknx.logger.color(CONSOLE_HEADLINE_COLOR);
    openknx.logger.log("============================= Help: Network Module =============================");
    openknx.logger.color(0);
    openknx.logger.log("Command(s)               Description");
    openknx.console.printHelpLine("net, n", "Show network information / connection state");
    openknx.console.printHelpLine("net ?, n ?", "Show this help");
    openknx.console.printHelpLine("net reset", "Re-initialise the network adapter (like a cable re-plug)");
#ifdef KNX_IP_WIFI
    openknx.console.printHelpLine("wifi <SSID> <PSK>", "Set the WiFi SSID and passphrase (reboots)");
    openknx.console.printHelpLine("erase wifi", "Erase the stored WiFi settings (reboots)");
#endif
#if MASK_VERSION == 0x091A || MASK_VERSION == 0x57B0
    openknx.console.printHelpLine("net mc", "Show the routing multicast address");
    openknx.console.printHelpLine("net mc <addr|reset>", "Set the routing multicast address (reboots)");
#endif
#ifdef KNX_IP_LAN
    openknx.console.printHelpLine("net eth", "Show the current ETH link mode");
#ifdef OPENKNX_ETH_AUTO_FALLBACK
    openknx.console.printHelpLine("net eth auto", "Auto-negotiation with auto-fallback ladder");
#else
    openknx.console.printHelpLine("net eth auto", "Auto-negotiation");
#endif
    openknx.console.printHelpLine("net eth 10 [half|full]", "Force a fixed 10 Mbit link (default half)");
    openknx.console.printHelpLine("net eth 100 [half|full]", "Force a fixed 100 Mbit link (default half)");
#endif
    openknx.logger.color(CONSOLE_HEADLINE_COLOR);
    openknx.logger.log("================================================================================");
    openknx.logger.color(0);
    openknx.logger.end();
}
#endif
