import { useState } from 'preact/hooks';
import { getJson } from '../common/api';
import { useInterval } from '../common/hooks';

function fmtUptime(s) {
  const d = Math.floor(s / 86400), h = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60;
  return (d ? d + 'd ' : '') + h + 'h ' + m + 'm';
}

function sourceText(d) {
  if (!d.signal) return 'no signal';
  const sacn = d.source === 'sacn';
  return `${sacn ? 'sACN' : 'Art-Net'} ${d.source_ip}` + (sacn ? ` p${d.priority}` : '') +
    (d.sources > 1 ? ` (+${d.sources - 1})` : '');
}

// Polls /api/status every second: [status, offline]. Shared by the start and settings pages.
export function useStatus() {
  const [status, setStatus] = useState(null);
  const [offline, setOffline] = useState(false);
  useInterval(async () => {
    try {
      setStatus(await getJson('/api/status'));
      setOffline(false);
    } catch (e) {
      setOffline(true);
    }
  }, 1000);
  return [status, offline];
}

export function Status({ status: s, offline, names }) {
  const w = s && s.wifi, d = s && s.dmx;
  const wifi = offline ? 'offline' : !s ? '–'
    : w.sta ? `${w.ssid} (${w.rssi} dBm)` : w.ap ? `AP: ${w.ap_ssid}` : 'connecting…';
  const ip = !s ? '–' : w.sta ? `${w.ip} · ${s.hostname}.local` : w.ap ? '192.168.4.1' : '–';
  const values = d ? d.values : '';

  return (
    <section class="card">
      <h2>Status</h2>
      <div class="grid">
        <div class="stat"><span>Wi-Fi</span><b>{wifi}</b></div>
        <div class="stat"><span>IP / host</span><b>{ip}</b></div>
        <div class="stat"><span>DMX input</span>
          <b><i class={'dot' + (d && d.signal ? ' on' : '')} /> {d ? sourceText(d) : '–'}</b></div>
        <div class="stat"><span>Packets/s (Art-Net / sACN)</span><b>{d ? `${d.artnet_pps} / ${d.sacn_pps}` : '–'}</b></div>
        <div class="stat"><span>DMX frames/s</span><b>{d ? d.fps : '–'}</b></div>
        <div class="stat"><span>Uptime</span><b>{s ? fmtUptime(s.uptime) : '–'}</b></div>
      </div>
      <div id="chan" title="DMX output, 512 channels">
        {Array.from({ length: 512 }, (_, i) => {
          const v = values ? parseInt(values.substr(i * 2, 2), 16) : 0;
          const n = names[i + 1];
          return (
            <div key={i} title={`Ch ${i + 1}${n ? ' ' + n : ''}: ${v}`}>
              <i style={{ height: v / 2.55 + '%' }} />
            </div>
          );
        })}
      </div>
      <div class="hint">Output channels 1–512 (hover for value)</div>
    </section>
  );
}
