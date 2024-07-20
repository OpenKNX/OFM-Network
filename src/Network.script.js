function sendWifiSettings(device, online, progress, context) {
    progress.setText("Übertrage die WiFi-Einstellungen.");
    online.connect();

    var data = [1];
    var ssid = device.getParameterByName("NET_WifiSSID").value;
    var psk = device.getParameterByName("NET_WifiPassword").value;

    data[1] = ssid.length;
    data[2] = psk.length;

    for (var i = 0; i < data[1]; ++i) {
        var code = ssid.charCodeAt(i);
        data = data.concat([code]);
    }
    data = data.concat(0); // null-terminated string

    for (var i = 0; i < data[2]; ++i) {
        var code = psk.charCodeAt(i);
        data = data.concat([code]);
    }
    data = data.concat(0); // null-terminated string

    var resp = online.invokeFunctionProperty(0xA0, 5, data);
    if (resp[0] != 0) {
        throw new Error("Fehler: Das verwendete Gerät unterstützt kein WiFi!");
    }

    device.getParameterByName("NET_WifiSSID").value = "";
    device.getParameterByName("NET_WifiPassword").value = "";
    online.disconnect();
    progress.setText("Übertragung der WiFi-Einstellungen abgeschlossen.");
}