// WiFi module (NETW): online functions of the "WiFi" page. All calls go to function property 160/5
// through BASE_invokeFunctionPropertyWrapper (OGM-Common), which splits request and response by APDU size.
// Commands: 1 = client credentials, 2 = AP password, 3 = start scan,
// 4 = scan state/result: [0, state, count, (rssi, security, channel, len, ssid)...], state 0 done, 1 running, 2 failed;
//     security 0 open, 1 WEP, 2 WPA, 3 WPA2, 4 WPA/WPA2, 5 WPA3, 6 WPA2/WPA3, 7 Enterprise, 255 unknown.

function NETW_utf8Encode(str) {
    var out = [];
    for (var i = 0; i < str.length; i++) {
        var c = str.charCodeAt(i);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < str.length) {
            c = 0x10000 + ((c - 0xD800) << 10) + (str.charCodeAt(++i) - 0xDC00);
        }
        if (c < 0x80) out.push(c);
        else if (c < 0x800) out.push(0xC0 | (c >> 6), 0x80 | (c & 0x3F));
        else if (c < 0x10000) out.push(0xE0 | (c >> 12), 0x80 | ((c >> 6) & 0x3F), 0x80 | (c & 0x3F));
        else out.push(0xF0 | (c >> 18), 0x80 | ((c >> 12) & 0x3F), 0x80 | ((c >> 6) & 0x3F), 0x80 | (c & 0x3F));
    }
    return out;
}

function NETW_utf8Decode(bytes, start, len) {
    var s = "";
    var i = start;
    var end = start + len;
    while (i < end) {
        var b = bytes[i++];
        var c;
        if (b < 0x80) c = b;
        else if ((b & 0xE0) == 0xC0 && i < end) c = ((b & 0x1F) << 6) | (bytes[i++] & 0x3F);
        else if ((b & 0xF0) == 0xE0 && i + 1 < end) { c = ((b & 0x0F) << 12) | ((bytes[i] & 0x3F) << 6) | (bytes[i + 1] & 0x3F); i += 2; }
        else if ((b & 0xF8) == 0xF0 && i + 2 < end) {
            c = ((b & 0x07) << 18) | ((bytes[i] & 0x3F) << 12) | ((bytes[i + 1] & 0x3F) << 6) | (bytes[i + 2] & 0x3F);
            i += 3;
            c -= 0x10000;
            s += String.fromCharCode(0xD800 + (c >> 10), 0xDC00 + (c & 0x3FF));
            continue;
        }
        else c = 0xFFFD;
        s += String.fromCharCode(c);
    }
    return s;
}

function NETW_call(data, device, online, progress) {
    var resp = BASE_invokeFunctionPropertyWrapper(0xA0, 5, data, device, online, progress);
    if (!resp || resp.length < 1) throw new Error("Keine Antwort vom Gerät.");
    if (resp[0] == 2) throw new Error("Das Gerät hat keine WiFi-Schnittstelle.");
    if (resp[0] != 0) throw new Error("Das Gerät hat die Anfrage abgelehnt.");
    return resp;
}

function NETW_sendClient(device, online, progress, context) {
    var ssid = NETW_utf8Encode(device.getParameterByName("NETW_ClientSsid").value);
    var psk = NETW_utf8Encode(device.getParameterByName("NETW_ClientPsk").value);
    if (ssid.length < 1 || ssid.length > 32) throw new Error("Netzname (SSID) mit 1 bis 32 Zeichen eingeben.");
    if (psk.length > 63 || (psk.length > 0 && psk.length < 8)) throw new Error("PSK leer lassen oder 8 bis 63 Zeichen.");

    progress.setText("Übertrage die WiFi-Zugangsdaten.");
    online.connect();
    var data = [1, ssid.length, psk.length].concat(ssid).concat([0]).concat(psk);
    NETW_call(data, device, online, progress);
    online.disconnect();

    device.getParameterByName("NETW_ClientSsid").value = "";
    device.getParameterByName("NETW_ClientPsk").value = "";
    NETW_fillRows(device, progress, [], false);
    progress.setText("WiFi-Zugangsdaten übertragen. Das Gerät startet neu.");
}

function NETW_sendApPassword(device, online, progress, context) {
    var pw = NETW_utf8Encode(device.getParameterByName("NETW_ApPassword").value);
    if (pw.length < 8 || pw.length > 63) throw new Error("Passwort mit 8 bis 63 Zeichen eingeben.");

    progress.setText("Übertrage das WiFi-Passwort.");
    online.connect();
    NETW_call([2, pw.length].concat(pw), device, online, progress);
    online.disconnect();

    device.getParameterByName("NETW_ApPassword").value = "";
    progress.setText("WiFi-Passwort übertragen.");
}

function NETW_scan(device, online, progress, context) {
    progress.setText("Gerät sucht nach Netzen.");
    NETW_fillRows(device, progress, [], false); // no stale result if the scan fails
    online.connect();
    NETW_call([3], device, online, progress);

    // The scan runs in the device background; poll until it reports done (state 0) or failed (2),
    // bounded by time: a poll over IP takes a few ms, over TP far longer.
    var resp = null;
    var start = new Date().getTime();
    while (new Date().getTime() - start < 20000) {
        resp = NETW_call([4], device, online, progress);
        progress.setProgress(Math.min(90, 10 + Math.floor((new Date().getTime() - start) / 250)));
        if (resp.length >= 3 && resp[1] != 1) break;
    }
    online.disconnect();
    if (!resp || resp.length < 3 || resp[1] == 1) throw new Error("Die Suche hat zu lange gedauert.");
    if (resp[1] == 2) throw new Error("Die Suche ist fehlgeschlagen.");

    var nets = [];
    var pos = 3;
    for (var n = 0; n < resp[2] && pos + 4 <= resp.length; n++) {
        var len = resp[pos + 3];
        nets.push({ ssid: NETW_utf8Decode(resp, pos + 4, len), rssi: resp[pos] > 127 ? resp[pos] - 256 : resp[pos],
                    security: resp[pos + 1], channel: resp[pos + 2] });
        pos += 4 + len;
    }
    NETW_fillRows(device, progress, nets, true);
}

// Writes up to 8 networks into the result rows and shows as many rows as were found;
// done = a finished scan, so an empty list shows "Kein Netz gefunden" on the page.
function NETW_fillRows(device, progress, nets, done) {
    var row = 0;
    for (; row < nets.length && row < 8; row++) {
        var rssi = nets[row].rssi;
        var sec = nets[row].security;
        device.getParameterByName("NETW_ScanSsid" + (row + 1)).value = nets[row].ssid;
        device.getParameterByName("NETW_ScanSig" + (row + 1)).value = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
        device.getParameterByName("NETW_ScanInfo" + (row + 1)).value = rssi + " dBm";
        device.getParameterByName("NETW_ScanChan" + (row + 1)).value = "" + nets[row].channel;
        device.getParameterByName("NETW_ScanSec" + (row + 1)).value = (sec <= 7) ? sec : 255;
    }
    for (var r = row; r < 8; r++) {
        device.getParameterByName("NETW_ScanSsid" + (r + 1)).value = "";
        device.getParameterByName("NETW_ScanInfo" + (r + 1)).value = "";
        device.getParameterByName("NETW_ScanChan" + (r + 1)).value = "";
        device.getParameterByName("NETW_ScanSec" + (r + 1)).value = 255;
        device.getParameterByName("NETW_ScanSig" + (r + 1)).value = 0;
    }
    device.getParameterByName("NETW_ScanCount").value = row;
    device.getParameterByName("NETW_ScanNone").value = (done && row == 0) ? 1 : 0;
    if (!done) return;
    progress.setProgress(100);
    progress.setText(row == 0 ? "Kein Netz gefunden." : row + " Netze gefunden.");
}

// Takes the network name of the chosen row and clears the list; a new scan brings it back.
function NETW_apply(device, online, progress, context) {
    var ssid = device.getParameterByName("NETW_ScanSsid" + context.Row).value;
    if (!ssid) return;
    device.getParameterByName("NETW_ClientSsid").value = ssid;
    NETW_fillRows(device, progress, [], false);
}
