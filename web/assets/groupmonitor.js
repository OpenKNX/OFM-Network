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
    fetch('/filemanager/download?path=%2Fopenknx_ga.tsv')
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

    // Busmonitor checkbox
    cbBusmon.onchange = function () {
        if (this.checked) {
            if (!confirm('Im Busmonitor-Modus werden keine KNX-Telegramme mehr verarbeitet und das Gerät kann nicht mehr am Bus teilnehmen.')) {
                this.checked = false;
                return;
            }
            if (ws && ws.readyState === 1) ws.send('busmonitor:on');
            statusRow('Busmonitor-Modus aktiviert &mdash; Telegramme werden nicht mehr verarbeitet', 'gm-sys-err');
        } else {
            if (ws && ws.readyState === 1) ws.send('busmonitor:off');
            statusRow('Busmonitor-Modus deaktiviert (TP-Reset)', 'gm-sys-ok');
        }
    };

    // Helper
    function p(n) { return (n < 10 ? '0' : '') + n; }
    function ts() {
        const d = new Date();
        return p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds()) + '.' + String(d.getMilliseconds()).padStart(3, '0');
    }
    const TC = { Write: 'gm-w', Read: 'gm-r', Response: 'gm-rsp' };
    function esc(s) { return String(s).replace(/[&<>]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c])); }

    function addRow(m) {
        if (!running) return;
        const tr = document.createElement('tr');
        const ga = (gaNames && m.addr != null) ? gaNames.get(m.addr) : null;
        tr.innerHTML = '<td>' + ts() + '</td><td class="gm-flag">' + (m.flags || '') + '</td>'
            + '<td>' + m.src + '</td><td>' + m.dst + '</td>'
            + '<td class="' + (TC[m.apci] || '') + '">' + m.apci + '</td>'
            + '<td>' + (ga ? esc(ga.hg) : '-') + '</td>'
            + '<td>' + (ga ? esc(ga.mg) : '-') + '</td>'
            + '<td>' + (ga ? esc(ga.nm) : '-') + '</td>'
            + '<td>' + (m.val ? esc(m.val) : '-') + '</td>'
            + '<td>' + (m.dpt || '-') + '</td>'
            + '<td class="gm-data">' + (m.hex || '') + '</td>'
            + '<td>' + m.len + '</td>';
        body.appendChild(tr);
        while (body.childNodes.length > MAX_ROWS) body.removeChild(body.firstChild);
        if (cbScroll.checked) wrap.scrollTop = wrap.scrollHeight;
    }

    let ws, reconnTimer;
    function statusRow(msg, cls) {
        const tr = document.createElement('tr');
        tr.className = 'gm-sysrow';
        tr.innerHTML = '<td>' + ts() + '</td><td colspan="11" class="gm-sysmsg ' + (cls || '') + '">' + msg + '</td>';
        body.appendChild(tr);
        if (cbScroll.checked) wrap.scrollTop = wrap.scrollHeight;
    }

    function connect() {
        ws = new WebSocket('ws://' + location.host + '/groupmonitor');
        ws.onopen = function () {
            st.textContent = 'Verbunden';
            st.className = 'gm-status ok';
            reconnDelay = 2000;
            statusRow('Verbunden', 'gm-sys-ok');
            // Query busmonitor state after connect
            fetch('/groupmonitor/state').then(r => r.json()).then(s => {
                cbBusmon.checked = !!s.monitoring;
                if (s.monitoring) statusRow('Busmonitor-Modus ist aktiv &mdash; Telegramme werden nicht verarbeitet', 'gm-sys-err');
            }).catch(() => {});
        };
        ws.onmessage = e => { try { addRow(JSON.parse(e.data)); } catch (err) {} };
        ws.onclose = () => {
            st.textContent = 'Getrennt — verbinde neu…';
            st.className = 'gm-status err';
            statusRow('Verbindung getrennt — verbinde neu…', 'gm-sys-err');
            clearTimeout(reconnTimer);
            reconnTimer = setTimeout(connect, reconnDelay);
            reconnDelay = Math.min(reconnDelay * 2, 30000);
        };
    }
    let reconnDelay = 2000;
    connect();
})();
