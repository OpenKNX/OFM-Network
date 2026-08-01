(function () {
    const out = document.getElementById('console-out');
    if (!out) return; // Stylesheet/Script sind global eingebunden

    const inp = document.getElementById('console-inp');
    const btn = document.getElementById('console-send');
    const MAX_LINES = 500;

    // Log-Zeilen kommen roh inkl. ANSI-Sequenzen an. Ausgabe nur über
    // textContent, nie als HTML.
    const ANSI = { 31: 'red', 32: 'green', 33: 'yellow', 90: 'gray' };

    function trim() {
        while (out.childNodes.length > MAX_LINES) out.removeChild(out.firstChild);
        out.scrollTop = out.scrollHeight;
    }

    function emit(frag, text, cls) {
        if (!text) return;
        if (cls) {
            const s = document.createElement('span');
            s.className = cls;
            s.textContent = text;
            frag.appendChild(s);
        } else {
            frag.appendChild(document.createTextNode(text));
        }
    }

    function appendOut(text) {
        const frag = document.createDocumentFragment();
        const re = /\x1b\[([0-9;]*)m/g;
        let cls = null, last = 0, m;
        while ((m = re.exec(text)) !== null) {
            emit(frag, text.slice(last, m.index), cls);
            // "1;32": letzter bekannter Code gewinnt, "0" setzt zurück
            cls = null;
            for (const c of m[1].split(';')) {
                const n = ANSI[parseInt(c, 10)];
                if (n) cls = n;
            }
            last = re.lastIndex;
        }
        emit(frag, text.slice(last), cls);
        out.appendChild(frag);
        trim();
    }

    function appendStatus(text, cls) {
        const s = document.createElement('span');
        s.className = cls;
        s.textContent = text + '\n';
        out.appendChild(s);
        trim();
    }

    // Kein Auto-Reconnect: das hat Logausgaben verwürfelt und die Ursache von
    // Verbindungsabbrüchen verschleiert.
    let ws;
    function connect() {
        ws = new WebSocket('ws://' + location.host + '/console');
        ws.onopen = () => { appendStatus('[verbunden]', 'green'); };
        ws.onmessage = e => { appendOut(e.data); };
        ws.onclose = () => { appendStatus('[Verbindung getrennt — Seite neu laden]', 'red'); };
    }
    connect();

    function send() {
        const v = inp.value.trim();
        inp.value = '';
        if (ws && ws.readyState === 1) ws.send(v);
    }

    btn.onclick = send;
    inp.addEventListener('keydown', e => {
        if (e.key === 'Enter') { e.preventDefault(); send(); }
    });
})();
