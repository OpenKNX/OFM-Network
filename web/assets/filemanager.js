async function uploadFile() {
    const file = document.getElementById('fi').files[0];
    if (!file) return;
    const base = currentDir === '/' ? '' : currentDir;
    const path = base + '/' + file.name;
    const CHUNK = 2048;
    const bar = document.getElementById('bar');
    const st = document.getElementById('st');
    const picker = document.getElementById('fm-picker');
    picker.classList.add('fm-hidden');
    bar.classList.remove('fm-hidden');
    bar.value = 0;
    st.textContent = '⚠︎ Upload läuft – Seite nicht verlassen';
    // Warn on navigation/link-click while upload is in flight (leaving aborts it)
    window.onbeforeunload = () => '';
    try {
        for (let off = 0; off < file.size || off === 0; off += CHUNK) {
            const end = Math.min(off + CHUNK, file.size);
            const r = await fetch('/filemanager/upload?path=' + encodeURIComponent(path) + '&offset=' + off + '&fs=' + currentFs,
                { method: 'POST', body: file.slice(off, end), headers: { 'Content-Type': 'application/octet-stream' } });
            if (!r.ok) {
                const msg = await r.text();
                throw new Error(r.status === 409 ? 'Datei existiert bereits – zuerst löschen'
                    : r.status === 413 ? 'Dateisystem voll – Speicherplatz reicht nicht aus'
                    : msg);
            }
            bar.value = Math.round(end / file.size * 100);
            if (end >= file.size) break;
        }
        st.textContent = '✓ Hochgeladen';
        setTimeout(() => location.reload(), 800);
    } catch (e) {
        st.textContent = 'Fehler: ' + e.message;
        bar.classList.add('fm-hidden');
        picker.classList.remove('fm-hidden');
    } finally {
        window.onbeforeunload = null;
    }
}

async function delFile(path) {
    if (!confirm('Datei löschen: ' + path + '?')) return;
    const r = await fetch('/filemanager/delete?path=' + encodeURIComponent(path) + '&fs=' + currentFs, { method: 'POST' });
    if (r.ok) location.reload(); else alert(await r.text());
}

async function delDir(path) {
    if (!confirm('Ordner löschen: ' + path + '?')) return;
    const r = await fetch('/filemanager/delete?path=' + encodeURIComponent(path) + '&fs=' + currentFs, { method: 'POST' });
    if (r.ok) location.reload(); else alert(await r.text());
}

async function mkDir() {
    const name = document.getElementById('nd').value.trim();
    if (!name) return;
    const base = currentDir === '/' ? '' : currentDir;
    const path = base + '/' + name;
    const r = await fetch('/filemanager/mkdir?path=' + encodeURIComponent(path) + '&fs=' + currentFs, { method: 'POST' });
    if (r.ok) location.reload(); else alert(await r.text());
}

{
    const _fi = document.getElementById('fi');
    const _btn = document.getElementById('fi-btn');
    const _nm = document.getElementById('fi-name');
    if (_fi && _btn && _nm) {
        _btn.addEventListener('click', () => _fi.click());
        _btn.addEventListener('keydown', e => { if (e.key === 'Enter' || e.key === ' ') _fi.click(); });
        _fi.addEventListener('change', function () {
            _nm.textContent = this.files[0] ? this.files[0].name : 'Keine Datei ausgewählt.';
        });
    }
};

// Load a file straight from a URL onto the drive of the current tab. The device downloads it itself,
// so nothing passes through the browser; the transfer is asynchronous and polled until it settles.
// The URL goes in the request BODY -- as a query parameter it overruns the server's request-line buffer.
async function fetchUrl() {
    const url = document.getElementById('fu').value.trim();
    const st = document.getElementById('fst');
    const btn = document.getElementById('fb');
    if (!url) { st.textContent = 'Erst eine URL eintragen.'; return; }
    const base = currentDir === '/' ? '' : currentDir;
    const name = url.split('?')[0].split('#')[0].split('/').pop() || 'download.bin';
    btn.disabled = true;
    st.textContent = name + ': wird angefordert \u2026';
    try {
        const r = await fetch('/filemanager/fetch?path=' + encodeURIComponent(base + '/' + name)
                            + '&fs=' + currentFs,
            { method: 'POST', body: url, headers: { 'Content-Type': 'text/plain' } });
        if (!r.ok) {
            // A rejected request may answer without a body (414 does) -- then the code is the message.
            const msg = (await r.text()).trim();
            st.textContent = 'Fehlgeschlagen: ' + (msg || ('HTTP ' + r.status));
            btn.disabled = false;
            return;
        }
    } catch (e) {
        st.textContent = 'Fehlgeschlagen: ' + e.message;
        btn.disabled = false;
        return;
    }
    const poll = setInterval(async () => {
        let s;
        try { s = await (await fetch('/filemanager/fetch/status')).json(); } catch (e) { return; }
        if (s.state === 'running') {
            st.textContent = name + ': ' + s.bytes + ' B' + (s.hops ? ' (' + s.hops + 'x weitergeleitet)' : '');
            return;
        }
        clearInterval(poll);
        btn.disabled = false;
        st.textContent = s.state === 'done'
            ? name + ': ' + s.bytes + ' B geladen'
            : 'Fehlgeschlagen: ' + (s.error || ('HTTP ' + s.status));
        if (s.state === 'done') setTimeout(() => location.reload(), 800);
    }, 700);
}
