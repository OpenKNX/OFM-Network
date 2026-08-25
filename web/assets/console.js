(function () {
    const out = document.getElementById('console-out');
    if (!out) return; // Stylesheet/Script sind global eingebunden

    const inp = document.getElementById('console-inp');
    const btn = document.getElementById('console-send');
    const MAX_LINES = 500;
    // Each message is wrapped in *one* node so the cap really counts lines: without the
    // wrapper it counted ANSI segments, and coloured output kept a fraction of the 500.
    // pre-wrap sits on the container, so an extra inline element changes no wrapping.

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
        const wrap = document.createElement('span');
        wrap.appendChild(frag);
        out.appendChild(wrap);
        trim();
    }

    function appendStatus(text, cls) {
        const s = document.createElement('span');
        s.className = cls;
        s.textContent = text + '\n';
        out.appendChild(s);
        trim();
    }

    // Der Socket kommt aus base.js und ist auf jeder Seite offen. Reconnect gibt
    // es dort, deshalb hier ein sichtbarer Marker: eine Lücke im Log soll man
    // sehen, statt sie stillschweigend hinzunehmen.
    OpenKNX.on('log', function (line) { appendOut(line); });
    OpenKNX.onState(function (state) {
        if (state === 'open') appendStatus('[verbunden]', 'green');
        else appendStatus('[Verbindung getrennt — Lücke im Log]', 'red');
    });

    function send() {
        // Clear the field only once the command is actually out -- during a reconnect it
        // would otherwise vanish without a word.
        if (OpenKNX.send('cmd', inp.value.trim())) inp.value = '';
    }

    btn.onclick = send;
    inp.addEventListener('keydown', e => {
        if (e.key === 'Enter') { e.preventDefault(); send(); }
    });
})();
