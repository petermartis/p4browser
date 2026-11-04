// Custom Homey control dashboard server (Pi5).
// Serves a tap-friendly page of real Homey on/off devices. Tapping a device
// calls /api/toggle, which calls the Homey REST API (PUT capability onoff).
// The Homey token stays on the Pi5 (never sent to the board/browser).
const http = require('http');
const fs = require('fs');
const path = require('path');

const HOMEY_HOST = process.env.HOMEY_HOST || '192.168.3.5';
// Create an API token in Homey (Settings -> API) and export it:
//   export HOMEY_TOKEN='<api-token>'
const HOMEY_TOKEN = process.env.HOMEY_TOKEN || '';
const PORT = 5051;

function homeyApi(pathname, method, body) {
  return new Promise((resolve, reject) => {
    const payload = body ? JSON.stringify(body) : null;
    const opts = {
      host: HOMEY_HOST, port: 80, path: pathname, method,
      headers: {
        'Authorization': 'Bearer ' + HOMEY_TOKEN,
        'Content-Type': 'application/json'
      }
    };
    if (payload) opts.headers['Content-Length'] = Buffer.byteLength(payload);
    const req = http.request(opts, (res) => {
      let b = '';
      res.on('data', (d) => b += d);
      res.on('end', () => {
        let parsed = b;
        try { parsed = b ? JSON.parse(b) : null; } catch (e) {}
        resolve({ status: res.statusCode, body: parsed });
      });
    });
    req.on('error', reject);
    req.setTimeout(10000, () => { req.destroy(new Error('homey timeout')); });
    if (payload) req.write(payload);
    req.end();
  });
}

const server = http.createServer(async (req, res) => {
  const u = req.url;
  const url = u.split('?')[0];
  try {
    if (url === '/' || url === '/index.html') {
      res.writeHead(200, { 'Content-Type': 'text/html' });
      res.end(fs.readFileSync(path.join(__dirname, 'dashboard.html')));
    } else if (url === '/api/devices') {
      const r = await homeyApi('/api/manager/devices/device/', 'GET');
      if (r.status !== 200) { res.writeHead(r.status, { 'Content-Type': 'application/json' }); res.end(JSON.stringify({ error: 'homey ' + r.status })); return; }
      const raw = r.body || {};
      const devices = Object.keys(raw).map(id => {
        const d = raw[id];
        const caps = d.capabilities || [];
        if (caps.indexOf('onoff') === -1) return null;
        const cobj = d.capabilitiesObj || {};
        const oo = cobj.onoff || {};
        return { id, name: d.name, on: !!oo.value };
      }).filter(Boolean);
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(devices));
    } else if (url === '/api/toggle') {
      const params = new URLSearchParams(u.split('?')[1] || '');
      const device = params.get('device');
      const on = params.get('on');
      if (!device || on === null) { res.writeHead(400, { 'Content-Type': 'application/json' }); res.end(JSON.stringify({ error: 'missing device or on' })); return; }
      const onState = (on === 'true');
      const r = await homeyApi('/api/manager/devices/device/' + encodeURIComponent(device) + '/capability/onoff', 'PUT', { value: onState });
      console.log('[TOGGLE] ' + device + ' on=' + onState + ' -> ' + r.status);
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ ok: r.status < 300, status: r.status }));
    } else {
      res.writeHead(404); res.end('not found');
    }
  } catch (e) {
    console.log('[ERR] ' + e.message);
    res.writeHead(500, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ error: e.message }));
  }
});

server.listen(PORT, '0.0.0.0', () => console.log('Homey dashboard server on :' + PORT));
