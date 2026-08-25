(function () {
    const body = document.getElementById('gm-body');
    if (!body) return; // Script ist global eingebunden — sonst WS auf jeder Seite

    const st = document.getElementById('gm-status');
    const cbScroll = document.getElementById('gm-scroll');
    const cbBusmon = document.getElementById('gm-busmon');
    const btnSS = document.getElementById('gm-startstop');
    const wrap = document.querySelector('.gm-tablewrap');
    const MAX_ROWS = 1000;
    let running = true;

    // GA-Namen client-seitig aus der TSV (Backend hält keine Namen vor)
    let gaNames = null;
    fetch('/groupmonitor/ga.tsv')
        .then(r => r.ok ? r.text() : Promise.reject())
        .then(t => {
            gaNames = new Map();
            t.split('\n').slice(1).forEach(l => {
                const c = l.split('\t');
                if (c.length >= 6) {
                    const a = parseInt(c[0], 10);
                    if (!isNaN(a)) gaNames.set(a, { hg: c[3].trim(), mg: c[4].trim(), nm: c[5].trim() });
                }
            });
        })
        .catch(() => {});

    // Start/Stop toggle
    function updateBtn() {
        if (running) { btnSS.textContent = 'Stop'; btnSS.className = 'running'; }
        else { btnSS.textContent = 'Start'; btnSS.className = 'stopped'; }
    }
    btnSS.onclick = () => {
        running = !running;
        updateBtn();
        statusRow(running ? 'Aufzeichnung gestartet' : 'Aufzeichnung gestoppt', running ? 'gm-sys-ok' : 'gm-sys-err');
    };
    updateBtn();
    document.getElementById('gm-clear').onclick = () => { body.innerHTML = ''; };

    // Busmonitor checkbox — absent on a router build (0x091A), see GroupMonitor.cpp
    if (cbBusmon) {
        cbBusmon.onchange = function () {
            if (this.checked) {
                if (!confirm('Im Busmonitor-Modus werden keine KNX-Telegramme mehr verarbeitet und das Gerät kann nicht mehr am Bus teilnehmen.')) {
                    this.checked = false;
                    return;
                }
                OpenKNX.send('busmon', '1');
                statusRow('Busmonitor-Modus aktiviert &mdash; Telegramme werden nicht mehr verarbeitet', 'gm-sys-err');
            } else {
                OpenKNX.send('busmon', '0');
                statusRow('Busmonitor-Modus deaktiviert (TP-Reset)', 'gm-sys-ok');
            }
        };
    }

    // Helper
    function p(n) { return (n < 10 ? '0' : '') + n; }
    function ts() {
        const d = new Date();
        return p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds()) + '.' + String(d.getMilliseconds()).padStart(3, '0');
    }
    const TC = { Write: 'gm-w', Read: 'gm-r', Response: 'gm-rsp' };
    function esc(s) { return String(s).replace(/[&<>]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c])); }

    // Firmware sendet die Flags einzeln; die Spalte bleibt kompakt als TDIERFBNA
    const FLAGS = [['transmitted', 'T'], ['addressed', 'D'], ['invalid', 'I'], ['extended', 'E'],
                   ['repeated', 'R'], ['filtered', 'F'], ['busy', 'B'], ['nack', 'N'], ['ack', 'A']];
    function flagStr(f) { return f ? FLAGS.map(([k, c]) => f[k] ? c : '_').join('') : ''; }

    function addRow(m) {
        if (!running) return;
        const tr = document.createElement('tr');
        const ga = (gaNames && m.ga) ? gaNames.get(m.dst.addr) : null;
        tr.innerHTML = '<td>' + ts() + '</td><td class="gm-flag">' + flagStr(m.flags) + '</td>'
            + '<td>' + m.src.text + '</td><td>' + m.dst.text + '</td>'
            + '<td class="' + (TC[m.apci] || '') + '">' + m.apci + '</td>'
            + '<td>' + (ga ? esc(ga.hg) : '-') + '</td>'
            + '<td>' + (ga ? esc(ga.mg) : '-') + '</td>'
            + '<td>' + (ga ? esc(ga.nm) : '-') + '</td>'
            + '<td>' + (m.value && m.value.text ? esc(m.value.text) : '-') + '</td>'
            + '<td>' + (m.value ? m.value.dpt : '-') + '</td>'
            + '<td class="gm-data">' + (m.hex || '') + '</td>'
            + '<td>' + m.len + '</td>';
        body.appendChild(tr);
        while (body.childNodes.length > MAX_ROWS) body.removeChild(body.firstChild);
        if (cbScroll.checked) wrap.scrollTop = wrap.scrollHeight;
    }

    function statusRow(msg, cls) {
        const tr = document.createElement('tr');
        tr.className = 'gm-sysrow';
        tr.innerHTML = '<td>' + ts() + '</td><td colspan="11" class="gm-sysmsg ' + (cls || '') + '">' + msg + '</td>';
        body.appendChild(tr);
        if (cbScroll.checked) wrap.scrollTop = wrap.scrollHeight;
    }

    // Socket und Reconnect kommen aus base.js. Den Busmonitor-Zustand liefert das
    // status-Event mit -- beim Verbinden und bei jeder Änderung, ein eigener Abruf
    // ist deshalb nicht mehr nötig.
    OpenKNX.on('tp', function (d) {
        try { addRow(JSON.parse(d)); } catch (err) {}
    });

    OpenKNX.on('status', function (d) {
        if (!cbBusmon) return;
        let s;
        try { s = JSON.parse(d); } catch (err) { return; }
        if (s.busmon === undefined) return;
        const was = cbBusmon.checked;
        cbBusmon.checked = !!s.busmon;
        if (s.busmon && !was) statusRow('Busmonitor-Modus ist aktiv &mdash; Telegramme werden nicht verarbeitet', 'gm-sys-err');
    });

    OpenKNX.onState(function (state) {
        if (state === 'open') {
            st.textContent = 'Verbunden';
            st.className = 'gm-status ok';
            statusRow('Verbunden', 'gm-sys-ok');
        } else {
            st.textContent = 'Getrennt — verbinde neu…';
            st.className = 'gm-status err';
            statusRow('Verbindung getrennt — verbinde neu…', 'gm-sys-err');
        }
    });
})();
