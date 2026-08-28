/* OpenKNX */
(function () {
    // Ein WebSocket für die gesamte Oberfläche, offen auf jeder Seite.
    // Nachrichten sind "key:payload", gesplittet wird am ersten ':' -- der Rest
    // bleibt roh, damit die Firmware nichts escapen muss.
    const handlers = {};
    const stateCbs = [];
    let ws = null;
    let state = null; // last state reported to the registrants, null = nothing yet
    let delay = 1000;
    let lastRx = 0;
    let pingAt = 0;   // wann zuletzt gefragt wurde, 0 = keine Frage offen
    let lastTick = 0; // erkennt einen gedrosselten Timer

    // performance.now(), not Date.now(): monotonic. A clock correction backwards would
    // make the differences below negative, leaving pingAt set forever and the liveness
    // check silently dead.
    const nowMs = (typeof performance !== 'undefined' && performance.now)
        ? function () { return performance.now(); }
        : function () { return Date.now(); };

    // Edge-triggered: with the network down every reconnect attempt fails again, and
    // reporting that as a fresh event filled the console with one gap marker per try.
    function fire(next) {
        if (next === state) return;
        state = next;
        stateCbs.forEach(function (cb) { try { cb(next); } catch (e) {} });
    }

    function reconnect() {
        setTimeout(connect, delay);
        delay = Math.min(delay * 2, 30000);
    }

    // Final teardown, used by onclose *and* by the liveness check. close() alone is not
    // enough there: a dead peer answers nothing, so the socket only moves to CLOSING and
    // onclose waits for a handshake that never comes -- up to the TCP timeout. Reporting
    // and reconnecting must not wait for that, or the page shows "connected" for another
    // 20 seconds after the device is gone.
    function fail() {
        if (!ws) return;
        const dead = ws;
        ws = null; // beat() and send() must see "no socket" from here on
        dead.onopen = dead.onmessage = dead.onclose = null; // no late callbacks
        try { dead.close(); } catch (e) {}
        pingAt = 0;
        fire('closed');
        reconnect();
    }

    function connect() {
        const scheme = location.protocol === 'https:' ? 'wss://' : 'ws://';
        // The constructor itself can throw (CSP connect-src, mixed content). Without
        // this catch the reconnect would end there for good -- onclose never fires.
        try {
            ws = new WebSocket(scheme + location.host + '/openknx/ws');
        } catch (e) {
            ws = null;
            fire('closed');
            reconnect();
            return;
        }
        ws.onopen = function () {
            delay = 1000;
            lastRx = lastTick = nowMs();
            pingAt = 0;
            fire('open');
        };
        ws.onmessage = function (e) {
            lastRx = nowMs();
            pingAt = 0;
            const i = e.data.indexOf(':');
            if (i < 0) return;
            const key = e.data.slice(0, i);
            const data = e.data.slice(i + 1);
            // Ein kaputter Handler darf den Socket nicht mitreissen.
            (handlers[key] || []).forEach(function (cb) { try { cb(data); } catch (err) {} });
        };
        ws.onclose = fail;
    }

    window.OpenKNX = {
        on: function (key, cb) { (handlers[key] = handlers[key] || []).push(cb); },
        off: function (key, cb) {
            const a = handlers[key];
            if (!a) return;
            const i = a.indexOf(cb);
            if (i >= 0) a.splice(i, 1);
        },
        send: function (key, data) {
            if (!ws || ws.readyState !== 1) return false;
            ws.send(key + ':' + (data === undefined ? '' : data));
            return true;
        },
        onState: function (cb) {
            stateCbs.push(cb);
            // A late registrant gets the current state, not the next change.
            if (state) try { cb(state); } catch (e) {}
        },
        get connected() { return !!ws && ws.readyState === 1; }
    };
    connect();

    // The direction the device cannot cover: a rebooted device sends neither FIN nor
    // RST, so the socket would stay open and the dot green until the page sends
    // something. Only an *unanswered ping* closes it, never silence alone -- a
    // background tab's timer is throttled to about once a minute.
    const CHECK_MS = 500;
    const IDLE_PING = 1000; // silence after which we ask
    const PONG_WAIT = 2000; // deadline for the reply -- on a LAN it is immediate

    function beat() {
        const now = nowMs();
        const slept = now - lastTick;
        lastTick = now;
        if (!ws || ws.readyState !== 1) return;

        // Timer was throttled or the machine slept: says nothing about the connection,
        // so just re-base.
        if (slept > CHECK_MS * 4) {
            lastRx = now;
            pingAt = 0;
            return;
        }

        if (pingAt) {
            if (now - pingAt > PONG_WAIT) fail();
            return;
        }
        if (now - lastRx > IDLE_PING) {
            pingAt = now;
            try { ws.send('ping:'); } catch (e) { pingAt = 0; }
        }
    }
    lastTick = nowMs();
    setInterval(beat, CHECK_MS);

    // Re-base when coming back to the foreground, so the first tick after throttling
    // does not judge on a stale interval.
    document.addEventListener('visibilitychange', function () {
        if (document.visibilityState !== 'visible') return;
        lastRx = lastTick = nowMs();
        pingAt = 0;
    });

    // ── Statusanzeige ─────────────────────────────────────────────────────
    // Die Nav gibt es auf jeder Seite, die ov-* Felder nur auf der Übersicht --
    // deshalb ist alles null-geprüft.
    const $ = function (id) { return document.getElementById(id); };
    function text(id, v) {
        const el = $(id);
        if (el && v !== undefined && v !== null) el.textContent = v;
    }
    function p2(n) { return (n < 10 ? '0' : '') + n; }

    // Uptime kommt nur beim Verbinden mit und läuft danach im Browser weiter --
    // sonst müsste sekündlich etwas gesendet werden, nur damit sie sich bewegt.
    let uptime = null;
    if ($('ov-up')) {
        setInterval(function () {
            if (uptime === null) return;
            uptime++;
            const d = Math.floor(uptime / 86400), r = uptime % 86400;
            text('ov-up', d + 'd ' + p2(Math.floor(r / 3600)) + ':' +
                          p2(Math.floor((r % 3600) / 60)) + ':' + p2(r % 60));
        }, 1000);
    }

    // Connection indicator in the navigation -- present on every page.
    OpenKNX.onState(function (state) {
        const dot = $('ws-dot');
        const open = state === 'open';
        if (dot) dot.className = 'prog-dot ' + (open ? 'dot-green' : 'dot-red');
        text('ws-text', open ? 'Verbunden' : 'Getrennt');
    });

    OpenKNX.on('status', function (d) {
        let s;
        try { s = JSON.parse(d); } catch (e) { return; }

        const dot = $('prog-dot');
        if (dot) dot.className = 'prog-dot ' + (s.prog ? 'dot-red' : 'dot-off');
        text('prog-text', s.prog ? 'Prog-Modus aktiv' : 'Prog-Modus inaktiv');
        const form = $('prog-form');
        if (form) form.action = '/prog?mode=' + (s.prog ? '0' : '1');

        const kdot = $('knx-dot');
        if (kdot) kdot.className = 'prog-dot ' + (s.cfg ? 'dot-green' : 'dot-off');
        text('knx-pa', s.pa);

        text('ov-pa', s.pa);
        const cfg = $('ov-cfg');
        if (cfg) {
            cfg.textContent = s.cfg ? '(Konfiguriert)' : '(Nicht konfiguriert)';
            cfg.className = s.cfg ? 'green' : 'gray';
        }
        text('ov-mem', s.mem);
        text('ov-cpu', s.cpu);
        ['ip', 'mask', 'gw', 'dns'].forEach(function (k) {
            text('ov-' + k, s.net ? s[k] : '–');
        });
        if (s.up !== undefined) uptime = s.up;
    });

    // Ohne JS bleibt der POST-Fallback; mit JS spart der Socket den Roundtrip.
    const progForm = $('prog-form');
    if (progForm) {
        progForm.addEventListener('submit', function (e) {
            if (!OpenKNX.connected) return; // dann greift der normale POST
            e.preventDefault();
            OpenKNX.send('prog', 't');
        });
    }
})();
