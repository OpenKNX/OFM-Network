/**
 * @file        NetworkMenu.cpp
 * @brief       OFM-Network on the device display: the menu "Network" with the IP setting
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
#if defined(DEVICE_DISPLAY_MODULE) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))

    #include "NetworkMenu.h"
    #include "DeviceDisplay.h"
    #include "OpenKNX/Network/Module.h"
    #include <new>

// plain functions, no lambdas: each lambda in a std::function brings its own handler, manager and type name
namespace
{
    using MenuOption = MenuConfig::MenuOption;
    OpenKNX::Network::LocalIpConfig s_edit; // the rows' edit, taken from the values they show on entry

    std::string dotted(IPAddress a)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (unsigned)a[0], (unsigned)a[1], (unsigned)a[2], (unsigned)a[3]);
        return std::string(buf);
    }

    std::string liveDhcp() { return openknxNetwork.localUsesStaticIp() ? "off" : "on"; }
    std::string liveIp() { return dotted(openknxNetwork.localIP()); }
    std::string liveMask() { return dotted(openknxNetwork.subnetMask()); }
    std::string liveGateway() { return dotted(openknxNetwork.gatewayIP()); }
    std::string liveDns() { return dotted(openknxNetwork.nameServerIP()); }

    IPAddress unpack(size_t p) { return IPAddress((uint8_t)(p >> 24), (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p); }

    void dhcpChanged(const MenuOption &, const MenuValue &v)
    {
        if (v.isBool()) s_edit.useStaticIP = !v.getBool(); // checked == DHCP
    }
    void ipChanged(const MenuOption &, const MenuValue &v) { s_edit.ip = unpack(v.getSizeT()); }
    void maskChanged(const MenuOption &, const MenuValue &v) { s_edit.subnet = unpack(v.getSizeT()); }
    void gatewayChanged(const MenuOption &, const MenuValue &v) { s_edit.gateway = unpack(v.getSizeT()); }

    void applyAction()
    {
        using Result = OpenKNX::Network::Module::LocalApplyResult;
        const char *message = "No change";
        switch (openknxNetwork.applyLocalNetworkConfig(s_edit))
        {
            case Result::Restarting: message = "Stored, restarting"; break;
            case Result::Unchanged: message = "No change"; break;
            case Result::NotProgrammed: message = "Not stored:\nnot programmed"; break;
            case Result::Invalid: message = "Not stored: check\nIP, mask, gateway"; break;
            case Result::SetupActive: message = "Not stored:\nWiFi AP active"; break;
            case Result::NotSupported: message = "Not stored:\nset by parameters"; break;
            case Result::UpdateRunning: message = "Not stored:\nupdate running"; break;
        }
        openknxDisplayModule.showToast(message); // lines of at most 21 chars (128 px / 6 px)
    }

    MenuOption &addRow(std::vector<MenuOption> &rows, const char *label, MenuConfig::MenuElementType type, const char *key = "")
    {
        rows.emplace_back();
        MenuOption &o = rows.back();
        o.label = label;
        o.type = type;
        o.key = key;
        return o;
    }

    void addIpRow(std::vector<MenuOption> &rows, const char *label, const char *key, IPAddress a,
                  void (*changed)(const MenuOption &, const MenuValue &))
    {
        MenuOption &o = addRow(rows, label, MenuConfig::IpAddress, key); // shows o.ip, or "auto" while net_dhcp is set
        for (uint8_t i = 0; i < 4; i++) o.ip[i] = a[i];
        o.onValueChanged = changed;
    }

    void addLiveRow(std::vector<MenuOption> &rows, const char *label, std::string (*live)())
    {
        addRow(rows, label, MenuConfig::Readonly).valueProvider = live;
    }

    std::vector<MenuOption> buildMenu()
    {
        const bool dhcp = !openknxNetwork.localUsesStaticIp();
        s_edit.useStaticIP = !dhcp; // edits not applied before are dropped
        s_edit.ip = openknxNetwork.localIP();
        s_edit.subnet = openknxNetwork.subnetMask();
        s_edit.gateway = openknxNetwork.gatewayIP();
    #ifdef OPENKNX_NETWORK_USEIPPROP
        const bool editable = !openknxNetwork.wifiApActive(); // the shown values are the AP's
    #else
        const bool editable = false; // the address comes from application parameters
    #endif

        std::vector<MenuOption> rows;
        rows.reserve(7);
        if (editable)
        {
            MenuOption &d = addRow(rows, "DHCP", MenuConfig::Checkbox, "net_dhcp"); // the key locks the IP rows to "auto"
            new (&d.defaultValue) MenuValue(dhcp);
            d.onValueChanged = dhcpChanged;
            addIpRow(rows, "IP address", "net_ip", s_edit.ip, ipChanged);
            addIpRow(rows, "Subnet mask", "net_subnet", s_edit.subnet, maskChanged);
            addIpRow(rows, "Gateway", "net_gw", s_edit.gateway, gatewayChanged);
        }
        else
        {
            addLiveRow(rows, "DHCP", liveDhcp);
            addLiveRow(rows, "IP address", liveIp);
            addLiveRow(rows, "Subnet mask", liveMask);
            addLiveRow(rows, "Gateway", liveGateway);
        }
        addLiveRow(rows, "DNS server", liveDns); // application parameter, not set here
        if (editable)
        {
            MenuOption &apply = addRow(rows, "Apply", MenuConfig::Action, "net_apply");
            apply.confirmText = "Restart to apply?";
            apply.action = applyAction;
        }
        addRow(rows, "Back", MenuConfig::Back);
        return rows;
    }
} // namespace

bool registerNetworkMenu()
{
    MenuOption root;
    root.label = "Network";
    root.type = MenuConfig::Submenu;
    root.key = "net_root";
    root.devOnly = true;
    root.sortOrder = 20;
    root.submenuBuilder = buildMenu;
    return openknxDisplayModule.tryRegisterRootItem(root);
}

#endif
