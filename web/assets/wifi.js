// WiFi page (/wifi): built from /wifi/status (polled every 2 s), /wifi/scan, /wifi/qr and /wifi/save.
(function () {
    var R = document.getElementById('wf');
    if (!R) return;
    var st = null, view = '', nets = null, sel = -1, scanning = false, failed = false, saved = '', qrUrl = '', qrDone = null;
    var LV = ['', 'schwach', 'mittel', 'gut', 'sehr gut'];
    var SEC = ['offen', 'WEP', 'WPA', 'WPA2', 'WPA/WPA2', 'WPA3', 'WPA2/WPA3', 'Enterprise'];
    var LK = '<svg class="wf-lk" viewBox="0 0 13 15"><path d="M3.5 6.5V4.5a3 3 0 0 1 6 0v2" fill="none" stroke="currentColor" stroke-width="1.8"/><rect x="1" y="6.5" width="11" height="8" rx="1.5" fill="currentColor"/></svg>';
    var LO = '<svg class="wf-lk" viewBox="0 0 13 15"><path d="M3.5 6.5V4.5a3 3 0 0 1 5.8-1.1" fill="none" stroke="currentColor" stroke-width="1.8"/><rect x="1.9" y="7.4" width="9.2" height="6.2" rx="1.2" fill="none" stroke="currentColor" stroke-width="1.8"/></svg>';
    var FORM = '<div class="wf-form" id="wf-f"><label for="wf-ss">Netzname</label><input type="text" id="wf-ss" maxlength="32" autocomplete="off">' +
        '<label for="wf-pw">Passwort</label><input type="password" id="wf-pw" maxlength="63" placeholder="8 bis 63 Zeichen">' +
        '<span></span><label><input type="checkbox" id="wf-sh"> Passwort anzeigen</label>' +
        '<div class="a"><button class="wf-btn" id="wf-sv">Speichern und verbinden</button></div></div><div id="wf-m"></div>';

    function $(i) { return document.getElementById(i) }
    function esc(s) { return String(s).replace(/[&<>"']/g, function (c) { return '&#' + c.charCodeAt(0) + ';' }) }
    function lvl(r) { return r >= -55 ? 4 : r >= -65 ? 3 : r >= -75 ? 2 : 1 }
    function bars(r) { return '<span class="wf-sig l' + lvl(r) + '"><i></i><i></i><i></i><i></i></span>' }
    function sec(c) { return c == 0 ? '<span class="wf-open">' + LO + 'offen</span>' : '<span class="wf-sec">' + LK + (SEC[c] || 'gesichert') + '</span>' }
    function dbm(r) { return '\u2212' + Math.abs(r) + ' dBm' }
    function dur(s) { var h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60); return h ? h + ' h ' + m + ' min' : m + ' min' }
    function row(k, v) { return '<tr><td>' + k + '</td><td>' + v + '</td></tr>' }
    function get(u) { return fetch(u).then(function (r) { if (!r.ok) throw r.status; return r.json() }) }
    function msg(t, warn) { var m = $('wf-m'); if (m) m.innerHTML = t ? '<div class="wf-msg' + (warn ? ' w' : '') + '">' + esc(t) + '</div>' : '' }
    function bar(cls, pill, items) {
        var h = '<div class="wf-bar ' + cls + '"><span class="wf-pill">' + pill + '</span>';
        items.forEach(function (it) { h += '<span><i>' + it[0] + '</i><b>' + it[1] + '</b></span>' });
        return h + '</div>';
    }

    // ---- QR code from /wifi/qr: n modules per side, bits row-major, 4 per hex digit ----
    function drawQr(cv, q) {
        var n = q.n, s = n + 8, c = cv.getContext('2d'); // 4-module quiet zone
        cv.width = s; cv.height = s;
        c.fillStyle = '#fff'; c.fillRect(0, 0, s, s); c.fillStyle = '#111';
        for (var i = 0; i < n * n; i++)
            if (parseInt(q.b[i >> 2], 16) & (8 >> (i & 3))) c.fillRect(i % n + 4, Math.floor(i / n) + 4, 1, 1);
    }
    function qr(id, url) {
        var cv = $(id);
        if (!cv) return;
        get('/wifi/qr?t=' + encodeURIComponent(url)).then(function (q) { drawQr(cv, q) }).catch(function () { cv.parentNode.hidden = true });
    }

    // ---- charts ----
    // Grid lines go into the stretched SVG, their labels sit on top as HTML so the text keeps its shape
    function grid(y, h, lines) {
        var g = '', l = '';
        lines.forEach(function (t) {
            var v = y(t[0]);
            g += '<line x1="0" x2="600" y1="' + v.toFixed(1) + '" y2="' + v.toFixed(1) + '" stroke="#e8e8e8" stroke-dasharray="3 4"/>';
            l += '<span style="top:' + (v * 100 / h).toFixed(1) + '%">' + t[1] + '</span>';
        });
        return [g, l];
    }
    function chart(cap, h, gl, g) { return '<div class="wf-ch"><div class="c">' + cap + '</div><div class="wf-cv"><svg viewBox="0 0 600 ' + h + '" preserveAspectRatio="none">' + gl[0] + g + '</svg>' + gl[1] + '</div></div>' }
    function rssiChart(h) {
        if (!h || !h.length) return '';
        var y = function (d) { return 6 + 68 * (-40 - Math.max(-90, Math.min(-40, d))) / 50 };
        var gl = grid(y, 80, [[-55, 'sehr gut'], [-65, 'gut'], [-75, 'mittel']]), g = '', seg = '', off = 60 - h.length;
        var line = function () { if (seg) g += '<polyline points="' + seg + '" fill="none" stroke="#449841" stroke-width="2" vector-effect="non-scaling-stroke"/>'; seg = '' };
        h.forEach(function (d, i) { if (d == 0) line(); else seg += ((i + off) * 600 / 59).toFixed(1) + ',' + y(d).toFixed(1) + ' ' });
        line();
        return chart('Signal der letzten 10 Minuten', 80, gl, g);
    }
    function pingBlock(p) {
        if (!p) return '';
        var h = '<h2>Verbindung zum WiFi-Router</h2>';
        if (!p.length) return h + '<p class="meta"><span class="wf-spin"></span>Messung startet \u2026</p>';
        var lost = 0, sum = 0, n = 0, mn = 1e9, mx = 0, off = 60 - p.length;
        var y = function (ms) { return 4 + 66 * (1 - Math.log(1 + Math.min(ms, 100)) / Math.log(101)) };
        var gl = grid(y, 70, [[10, '10 ms'], [50, '50 ms']]), g = '';
        p.forEach(function (ms, i) {
            var t = ms < 0 ? 0 : y(ms);
            if (ms < 0) lost++; else { sum += ms; n++; mn = Math.min(mn, ms); mx = Math.max(mx, ms) }
            g += '<rect x="' + ((i + off) * 10 + 1) + '" y="' + t.toFixed(1) + '" width="8" height="' + (70 - t).toFixed(1) + '" fill="' + (ms < 0 ? '#c0392b' : '#449841') + '"' + (ms < 0 || i == p.length - 1 ? '' : ' fill-opacity=".55"') + '/>';
        });
        var cur = p[p.length - 1];
        return h + '<table class="attribute-table">' + row('WiFi-Router', esc(st.gw)) +
            row('Antwortzeit', (cur < 0 ? '<span class="red">keine Antwort</span>' : cur + ' ms') + (n ? ' <span class="gray">Mittel ' + Math.round(sum / n) + ' ms, ' + mn + ' bis ' + mx + ' ms</span>' : '')) +
            row('Verloren', lost + ' von ' + p.length + ' (' + (lost * 100 / p.length).toFixed(1).replace('.', ',') + ' %)') + '</table>' +
            chart('Ein Ping pro Sekunde, letzte 60 s \u00b7 rot = keine Antwort', 70, gl, g) + '<p class="meta">L\u00e4uft nur, solange diese Seite offen ist.</p>';
    }

    // ---- network list ----
    function netTable() {
        var el = $('wf-n');
        if (!el) return;
        if (scanning) { el.innerHTML = '<p class="meta"><span class="wf-spin"></span>Suche l\u00e4uft \u2026</p>'; return }
        if (!nets) { el.innerHTML = ''; return }
        if (!nets.length) { el.innerHTML = '<p class="meta">' + (failed ? 'Die Suche ist fehlgeschlagen.' : 'Kein Netz gefunden.') + '</p>'; return }
        var h = '<div class="wf-nets"><table><thead><tr><th>Signal</th><th>Netzname</th><th class="wf-hide">Pegel</th><th>Kanal</th><th>Sicherheit</th></tr></thead><tbody>';
        nets.forEach(function (n, i) {
            h += '<tr data-i="' + i + '"' + (i == sel ? ' class="sel"' : '') + '><td>' + bars(n.rssi) + '</td><td class="n">' + esc(n.ssid) + '</td><td class="wf-hide">' + dbm(n.rssi) + '</td><td>' + n.ch + '</td><td>' + sec(n.sec) + '</td></tr>';
        });
        el.innerHTML = h + '</tbody></table></div>';
        el.querySelectorAll('tr[data-i]').forEach(function (tr) {
            tr.onclick = function () {
                sel = +tr.getAttribute('data-i');
                $('wf-ss').value = nets[sel].ssid;
                $('wf-pw').placeholder = nets[sel].sec == 0 ? 'ohne Passwort' : '8 bis 63 Zeichen';
                netTable();
                $('wf-pw').focus();
            };
        });
    }
    function scan(start) {
        scanning = true; sel = -1; netTable();
        get('/wifi/scan' + (start ? '?start=1' : '')).then(function (j) {
            if (j.running) { setTimeout(function () { scan(0) }, 1000); return }
            scanning = false; failed = !!j.failed; nets = j.networks; netTable();
        }).catch(function () { scanning = false; failed = true; nets = []; netTable() });
    }

    // ---- save ----
    function bindForm() {
        $('wf-sh').onchange = function () { $('wf-pw').type = this.checked ? 'text' : 'password' };
        $('wf-sv').onclick = function () {
            var ssid = $('wf-ss').value;
            msg('');
            fetch('/wifi/save?ssid=' + encodeURIComponent(ssid) + '&psk=' + encodeURIComponent($('wf-pw').value), { method: 'POST' })
                .then(function (r) { return r.text().then(function (t) { return { ok: r.ok, t: t } }) })
                .then(function (r) {
                    if (!r.ok) { msg(r.t, true); return }
                    if (st.mode == 'setup') { saved = ssid; render(); return }
                    msg(r.t);
                }).catch(function () { msg('Das Ger\u00e4t antwortet nicht.', true) });
        };
    }

    // ---- views ----
    function skeleton() {
        var m = st.mode;
        if (m == 'setup')
            R.innerHTML = '<h1>WiFi einrichten</h1><div id="wf-s"></div><p class="meta">Netz w\u00e4hlen, Passwort eingeben und speichern. Das Ger\u00e4t startet danach neu und verbindet sich mit dem gew\u00e4hlten Netz.</p>' +
                '<h2>Netz w\u00e4hlen<button class="wf-btn" id="wf-sc">Neu suchen</button></h2><div id="wf-n"></div><h2>Zugangsdaten</h2>' + FORM +
                '<p class="meta">Danach ist dieses Einrichtungs-WiFi nicht mehr da. Das Ger\u00e4t ist dann im gew\u00e4hlten Netz erreichbar.</p>';
        else if (m == 'own')
            R.innerHTML = '<h1>WiFi</h1><div id="wf-s"></div><div class="wf-conn"><div><table class="attribute-table" id="wf-t"></table>' +
                '<p class="meta" style="margin-top:12px">Name, Kanal und Passwort werden bei der Programmierung festgelegt.</p></div>' +
                '<div class="wf-qr"><canvas id="wf-q"></canvas>Diese Seite auf dem Handy \u00f6ffnen</div></div>';
        else
            R.innerHTML = '<h1>WiFi</h1><div id="wf-s"></div><div class="wf-conn"><div><table class="attribute-table" id="wf-t"></table><div id="wf-rc"></div></div>' +
                '<div class="wf-qr"><canvas id="wf-q"></canvas>Diese Seite auf dem Handy \u00f6ffnen</div></div><div id="wf-p"></div>' +
                '<h2>Netz wechseln<button class="wf-btn" id="wf-sc">Netze suchen</button></h2><div id="wf-lock"></div><div id="wf-n"></div>' + FORM;
        if ($('wf-f')) bindForm();
        if ($('wf-sc')) $('wf-sc').onclick = function () { scan(1) };
        if (m != 'setup') qr('wf-q', location.href.split('#')[0]);
        if (m == 'setup') {
            scan(1);
            if (st.mdns) get('/wifi/qr?t=' + encodeURIComponent('http://' + st.host + '.local')).then(function (q) { qrDone = q }).catch(function () {});
        }
    }
    function update() {
        var m = st.mode, s = $('wf-s'), t = $('wf-t');
        if (m == 'setup') s.innerHTML = bar('b', 'Einrichtungs-WiFi', [['Netzname', esc(st.ssid)], ['Ger\u00e4t', esc(st.ip)]]);
        else if (m == 'own') {
            s.innerHTML = bar('', 'Eigenes WiFi', [['Netzname', esc(st.ssid)], ['Ger\u00e4te', st.clients + ' verbunden'], ['Kanal', st.ch]]);
            t.innerHTML = row('Netzname', esc(st.ssid)) + row('Sicherheit', 'WPA2') + row('Kanal', st.ch + (st.ch > 14 ? ' (5 GHz)' : ' (2,4 GHz)')) +
                row('Adresse', esc(st.ip)) + row('Verbundene Ger\u00e4te', st.clients);
        } else {
            var on = m == 'client';
            s.innerHTML = on ? bar('', 'Verbunden', [['Netzname', esc(st.ssid)], ['Signal', bars(st.rssi) + LV[lvl(st.rssi)]], ['Kanal', st.ch]])
                : st.ssid ? bar('w', 'Keine Verbindung', [['Netzname', esc(st.ssid)], ['', 'neuer Versuch l\u00e4uft']]) : bar('w', 'Keine Zugangsdaten', []);
            t.innerHTML = row('Netzname', esc(st.ssid)) + (on ? row('Signal', bars(st.rssi) + LV[lvl(st.rssi)] + ' (' + dbm(st.rssi) + ')') +
                row('Kanal', st.ch + (st.ch > 14 ? ' (5 GHz)' : ' (2,4 GHz)')) + row('Sicherheit', SEC[st.sec] || '\u2013') +
                row('Adresse', esc(st.ip)) + (st.mdns ? row('Name im Netz', esc(st.host) + '.local') : '') + row('Verbunden seit', dur(st.up)) : '');
            $('wf-rc').innerHTML = rssiChart(st.rh);
            $('wf-p').innerHTML = on && st.gw != '0.0.0.0' ? pingBlock(st.ph) : '';
            $('wf-lock').innerHTML = st.prog ? '' : '<div class="wf-msg w">Nur im Prog-Modus m\u00f6glich: Prog-Taste am Ger\u00e4t dr\u00fccken oder \u201eProg-Modus\u201c im Men\u00fc einschalten.</div>';
            $('wf-f').hidden = !st.prog;
            $('wf-sc').hidden = !st.prog;
            if (!st.prog && nets) { nets = null; netTable() }
        }
    }
    function done() {
        R.innerHTML = '<div class="wf-done"><div class="t">\u2713</div><h1>Gespeichert</h1><p>Das Ger\u00e4t startet neu und verbindet sich mit <b>' + esc(saved) + '</b>.</p>' +
            '<ol><li>Dieses WiFi verschwindet in wenigen Sekunden.</li><li>Das Handy wieder mit <b>' + esc(saved) + '</b> verbinden.</li><li>Das Ger\u00e4t ist dann hier erreichbar:</li></ol>' +
            (st.mdns ? '<div class="wf-qr"><canvas id="wf-q"></canvas><b>http://' + esc(st.host) + '.local</b></div>' : '') +
            '<p class="meta">' + (st.mdns ? 'Klappt der Name nicht, ' : '') + 'steht die Adresse ' + (st.disp ? 'am Display des Ger\u00e4ts oder ' : '') + 'in der Ger\u00e4teliste des WiFi-Routers.</p>' +
            '<div class="wf-msg w">' + (st.cfg ? 'Kommt keine Verbindung zustande, die Prog-Taste am Ger\u00e4t dr\u00fccken: Das Einrichtungs-WiFi startet dann wieder.'
                : 'Kommt keine Verbindung zustande (z. B. falsches Passwort), \u00f6ffnet das Ger\u00e4t nach 30 s wieder dieses Einrichtungs-WiFi.') + '</div></div>';
        var cv = $('wf-q');
        if (!cv) return;
        if (qrDone) drawQr(cv, qrDone); else cv.parentNode.removeChild(cv);
    }
    function render() {
        if (saved) { if (view != 'done') { view = 'done'; done() } return }
        var v = st.mode == 'offline' ? 'client' : st.mode; // same page, only the values differ
        if (view != v) { view = v; skeleton() }
        update();
    }
    function poll() {
        get('/wifi/status').then(function (j) { st = j; render() }).catch(function () {}).then(function () { setTimeout(poll, 2000) });
    }
    poll();
})();
