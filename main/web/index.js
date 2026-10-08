const $ = id => document.getElementById(id);
const fields = ['wifi_ssid','protocol','artnet_universe','sacn_universe','on_loss','loss_timeout_ms',
                'refresh_hz','name','hostname','tx_pin','de_pin','uart','led_pin'];
const numeric = new Set(['protocol','artnet_universe','sacn_universe','on_loss','loss_timeout_ms',
                         'refresh_hz','tx_pin','de_pin','uart','led_pin']);
let resetArmed = false;

const chan = $('chan'), bars = [];
for (let i = 0; i < 512; i++) {
  const d = document.createElement('div'), b = document.createElement('i');
  d.appendChild(b); chan.appendChild(d); bars.push([d, b]);
}

function msg(t, bad) { $('msg').textContent = t; $('msg').style.color = bad ? 'var(--bad)' : 'var(--ok)'; }

function artHint() {
  const v = +$('artnet_universe').value || 0;
  $('art_hint').textContent = `Net ${v >> 8} · Sub ${(v >> 4) & 15} · Uni ${v & 15} (QLC+ universe ${v})`;
}
$('artnet_universe').addEventListener('input', artHint);

function fmtUp(s) {
  const d = Math.floor(s / 86400), h = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60;
  return (d ? d + 'd ' : '') + h + 'h ' + m + 'm';
}

async function loadConfig() {
  const c = await (await fetch('/api/config')).json();
  for (const k of fields) if (k in c) $(k).value = c[k];
  $('wifi_pass').value = '';
  $('pass_hint').textContent = c.wifi_pass_set ? 'Saved – leave empty to keep' : '';
  artHint();
}

async function poll() {
  try {
    const s = await (await fetch('/api/status')).json();
    $('t_name').textContent = s.name; document.title = s.name; $('t_fw').textContent = 'v' + s.fw;
    const w = s.wifi;
    $('s_wifi').textContent = w.sta ? `${w.ssid} (${w.rssi} dBm)` : (w.ap ? `AP: ${w.ap_ssid}` : 'connecting…');
    $('s_ip').textContent = w.sta ? `${w.ip} · ${s.hostname}.local` : (w.ap ? '192.168.4.1' : '–');
    const d = s.dmx;
    $('s_dot').className = 'dot' + (d.signal ? ' on' : '');
    $('s_src').textContent = d.signal ? `${d.source === 'sacn' ? 'sACN' : 'Art-Net'} ${d.source_ip}` +
      (d.source === 'sacn' ? ` p${d.priority}` : '') + (d.sources > 1 ? ` (+${d.sources - 1})` : '') : 'no signal';
    $('s_pps').textContent = `${d.artnet_pps} / ${d.sacn_pps}`;
    $('s_fps').textContent = d.fps;
    $('s_up').textContent = fmtUp(s.uptime);
    $('fw_info').textContent = `Running v${s.fw} from partition ${s.partition}`;
    for (let i = 0; i < 512; i++) {
      const v = parseInt(d.values.substr(i * 2, 2), 16);
      bars[i][1].style.height = (v / 2.55) + '%';
      bars[i][0].title = `Ch ${i + 1}${chNames[i + 1] ? ' ' + chNames[i + 1] : ''}: ${v}`;
    }
  } catch (e) {
    $('s_wifi').textContent = 'offline';
  }
}

$('scan').onclick = async () => {
  $('scan_msg').textContent = 'Scanning…';
  try {
    const list = await (await fetch('/api/scan')).json();
    $('ssids').innerHTML = '';
    for (const n of list) {
      const o = document.createElement('option');
      o.value = n.ssid; o.label = `${n.rssi} dBm · ch ${n.ch}${n.secure ? ' · 🔒' : ''}`;
      $('ssids').appendChild(o);
    }
    $('scan_msg').textContent = list.length ? `${list.length} networks – pick one in the SSID field` : 'No networks found';
  } catch (e) { $('scan_msg').textContent = 'Scan failed'; }
};

$('f').onsubmit = async ev => {
  ev.preventDefault();
  const body = {};
  for (const k of fields) body[k] = numeric.has(k) ? +$(k).value : $(k).value;
  const p = $('wifi_pass').value;
  if (p) body.wifi_pass = p;
  try {
    const r = await (await fetch('/api/config', {method: 'POST', headers: {'Content-Type': 'application/json'},
                                                 body: JSON.stringify(body)})).json();
    if (!r.ok) return msg(r.error || 'Error', true);
    msg(r.reboot ? 'Saved – restarting…' : 'Saved');
    if (r.reboot) setTimeout(() => location.reload(), 8000); else loadConfig();
  } catch (e) { msg('Save failed', true); }
};

$('reboot').onclick = async () => {
  await fetch('/api/reboot', {method: 'POST'}); msg('Restarting…');
  setTimeout(() => location.reload(), 8000);
};

$('reset').onclick = async () => {
  if (!resetArmed) { resetArmed = true; $('reset').textContent = 'Click again to erase all settings';
    setTimeout(() => { resetArmed = false; $('reset').textContent = 'Factory reset'; }, 4000); return; }
  await fetch('/api/factory_reset', {method: 'POST'});
  msg('Settings erased – the bridge restarts in provisioning (AP) mode.');
};

$('fw_upload').onclick = () => {
  const f = $('fw_file').files[0], m = $('fw_msg'), pr = $('fw_prog');
  const say = (t, bad) => { m.textContent = t; m.style.color = bad ? 'var(--bad)' : ''; };
  if (!f) return say('Choose a .bin file first', true);
  if (!f.name.endsWith('.bin')) return say('Expected a .bin firmware file', true);
  const x = new XMLHttpRequest();
  x.open('POST', '/api/ota');
  x.setRequestHeader('Content-Type', 'application/octet-stream');
  x.upload.onprogress = e => { if (e.lengthComputable) { pr.value = e.loaded / e.total * 100;
    say(e.loaded < e.total ? `Uploading ${Math.round(pr.value)}%` : 'Verifying…'); } };
  x.onload = () => {
    $('fw_upload').disabled = false;
    let r = {}; try { r = JSON.parse(x.responseText); } catch (e) {}
    if (x.status === 200 && r.ok) {
      say('Installed – restarting…'); setTimeout(() => location.reload(), 10000);
    } else { pr.hidden = true; say(r.error || `Upload failed (HTTP ${x.status})`, true); }
  };
  x.onerror = () => { $('fw_upload').disabled = false; pr.hidden = true; say('Upload failed (connection lost)', true); };
  $('fw_upload').disabled = true; pr.hidden = false; pr.value = 0; say('Uploading…');
  x.send(f);
};

let chNames = {};
fetch('/api/names').then(r => r.json()).then(r => { chNames = r.names; }).catch(() => {});

loadConfig();
poll();
setInterval(poll, 1000);
