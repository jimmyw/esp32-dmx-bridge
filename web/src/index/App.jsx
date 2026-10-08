import { useEffect, useState } from 'preact/hooks';
import { getJson, postJson } from '../common/api';
import { useConfirm } from '../common/hooks';
import { Firmware } from './Firmware';
import { Status, useStatus } from './Status';

const FIELDS = ['wifi_ssid', 'protocol', 'artnet_universe', 'sacn_universe', 'on_loss', 'loss_timeout_ms',
                'refresh_hz', 'name', 'hostname', 'tx_pin', 'de_pin', 'uart', 'led_pin'];
const NUMERIC = new Set(['protocol', 'artnet_universe', 'sacn_universe', 'on_loss', 'loss_timeout_ms',
                         'refresh_hz', 'tx_pin', 'de_pin', 'uart', 'led_pin']);

function artHint(v) {
  v = +v || 0;
  return `Net ${v >> 8} · Sub ${(v >> 4) & 15} · Uni ${v & 15} (QLC+ universe ${v})`;
}

export function App() {
  const [status, offline] = useStatus();
  const [names, setNames] = useState({});
  const [form, setForm] = useState(Object.fromEntries(FIELDS.map(k => [k, ''])));
  const [pass, setPass] = useState('');
  const [passSet, setPassSet] = useState(false);
  const [networks, setNetworks] = useState([]);
  const [scanMsg, setScanMsg] = useState('');
  const [msg, setMsg] = useState({ text: '', bad: false });
  const [resetArmed, tapReset] = useConfirm(4000);

  const loadConfig = async () => {
    const c = await getJson('/api/config');
    setForm(f => Object.fromEntries(FIELDS.map(k => [k, k in c ? String(c[k]) : f[k]])));
    setPass('');
    setPassSet(!!c.wifi_pass_set);
  };
  useEffect(() => {
    loadConfig().catch(() => {});
    getJson('/api/names').then(r => setNames(r.names)).catch(() => {});
  }, []);

  const name = status ? status.name : 'DMX Bridge';
  useEffect(() => { document.title = name; }, [name]);

  const say = (text, bad = false) => setMsg({ text, bad });
  // Props for a form control bound to `form[k]`.
  const bind = k => ({
    id: k, name: k, value: form[k],
    onInput: e => { const v = e.currentTarget.value; setForm(f => ({ ...f, [k]: v })); },
  });

  const scan = async () => {
    setScanMsg('Scanning…');
    try {
      const list = await getJson('/api/scan');
      setNetworks(list);
      setScanMsg(list.length ? `${list.length} networks – pick one in the SSID field` : 'No networks found');
    } catch (e) {
      setScanMsg('Scan failed');
    }
  };

  const save = async ev => {
    ev.preventDefault();
    const body = Object.fromEntries(FIELDS.map(k => [k, NUMERIC.has(k) ? +form[k] : form[k]]));
    if (pass) body.wifi_pass = pass;
    try {
      const r = await postJson('/api/config', body);
      if (!r.ok) return say(r.error || 'Error', true);
      say(r.reboot ? 'Saved – restarting…' : 'Saved');
      if (r.reboot) setTimeout(() => location.reload(), 8000);
      else loadConfig();
    } catch (e) {
      say('Save failed', true);
    }
  };

  const reboot = async () => {
    await fetch('/api/reboot', { method: 'POST' });
    say('Restarting…');
    setTimeout(() => location.reload(), 8000);
  };

  const reset = async () => {
    if (!tapReset()) return;
    await fetch('/api/factory_reset', { method: 'POST' });
    say('Settings erased – the bridge restarts in provisioning (AP) mode.');
  };

  return (
    <main>
      <h1><span>{name}</span> <span class="pill">{status ? 'v' + status.fw : ''}</span>
        <a href="/console" style={{ marginLeft: 'auto' }}><button type="button">Open fader console</button></a></h1>

      <Status status={status} offline={offline} names={names} />

      <form onSubmit={save}>
        <section class="card">
          <h2>Wi-Fi</h2>
          <div class="row">
            <div><label for="wifi_ssid">Network (SSID)</label>
              <input {...bind('wifi_ssid')} list="ssids" autocomplete="off" />
              <datalist id="ssids">
                {networks.map(n => (
                  <option key={n.ssid} value={n.ssid}
                          label={`${n.rssi} dBm · ch ${n.ch}${n.secure ? ' · 🔒' : ''}`} />
                ))}
              </datalist></div>
            <div><label for="wifi_pass">Password</label>
              <input id="wifi_pass" name="wifi_pass" type="password" autocomplete="new-password" value={pass}
                     onInput={e => setPass(e.currentTarget.value)} />
              <div class="hint">{passSet ? 'Saved – leave empty to keep' : ''}</div></div>
          </div>
          <div class="actions">
            <button type="button" class="sec" onClick={scan}>Scan networks</button>
            <span class="hint">{scanMsg}</span>
          </div>
        </section>

        <section class="card">
          <h2>DMX input</h2>
          <div class="row">
            <div><label for="protocol">Protocol</label>
              <select {...bind('protocol')}>
                <option value="3">Art-Net + sACN</option>
                <option value="1">Art-Net only</option>
                <option value="2">sACN (E1.31) only</option>
              </select></div>
            <div><label for="artnet_universe">Art-Net universe (port-address)</label>
              <input {...bind('artnet_universe')} type="number" min="0" max="32767" />
              <div class="hint">{artHint(form.artnet_universe)}</div></div>
            <div><label for="sacn_universe">sACN universe</label>
              <input {...bind('sacn_universe')} type="number" min="1" max="63999" /></div>
          </div>
          <div class="row">
            <div><label for="on_loss">On signal loss</label>
              <select {...bind('on_loss')}>
                <option value="0">Hold last look</option>
                <option value="1">Blackout</option>
              </select></div>
            <div><label for="loss_timeout_ms">Loss timeout (ms)</label>
              <input {...bind('loss_timeout_ms')} type="number" min="500" max="60000" step="100" /></div>
            <div><label for="refresh_hz">DMX refresh (Hz)</label>
              <input {...bind('refresh_hz')} type="number" min="1" max="44" /></div>
          </div>
        </section>

        <section class="card">
          <h2>Device &amp; hardware</h2>
          <div class="row">
            <div><label for="name">Name</label><input {...bind('name')} maxLength={63} /></div>
            <div><label for="hostname">Hostname (.local)</label><input {...bind('hostname')} maxLength={31} /></div>
          </div>
          <div class="row">
            <div><label for="tx_pin">DMX TX GPIO</label>
              <input {...bind('tx_pin')} type="number" min="0" max="48" /></div>
            <div><label for="de_pin">RS485 DE GPIO</label>
              <input {...bind('de_pin')} type="number" min="-1" max="48" />
              <div class="hint">-1 = auto-direction module</div></div>
            <div><label for="uart">UART</label>
              <select {...bind('uart')}><option value="1">1</option><option value="2">2</option></select></div>
            <div><label for="led_pin">Status LED GPIO</label>
              <input {...bind('led_pin')} type="number" min="-1" max="48" />
              <div class="hint">-1 = none</div></div>
          </div>
          <div class="hint">Wi-Fi, hostname and pin changes restart the bridge.</div>
        </section>

        <Firmware status={status} />

        <div class="actions card">
          <button type="submit">Save</button>
          <button type="button" class="sec" onClick={reboot}>Restart</button>
          <button type="button" class="danger" onClick={reset}>
            {resetArmed ? 'Click again to erase all settings' : 'Factory reset'}
          </button>
          <span id="msg" style={{ color: msg.bad ? 'var(--bad)' : 'var(--ok)' }}>{msg.text}</span>
        </div>
      </form>
    </main>
  );
}
