import { useEffect, useState } from 'react';
import type { HostBridge } from '../bridge/contracts';
import styles from './LiveAudioMeter.module.css';

export function LiveAudioMeter({ bridge, endpointId, label, available }: {
  bridge: HostBridge; endpointId: string; label: string; available: boolean;
}) {
  const [peak, setPeak] = useState(0);
  const [failed, setFailed] = useState(false);
  useEffect(() => {
    let active = true;
    let timer: ReturnType<typeof setTimeout> | undefined;
    setPeak(0); setFailed(false);
    async function sample() {
      if (!active) return;
      let delay = 80;
      if (document.hidden) { setPeak(0); timer = setTimeout(sample, 500); return; }
      try {
        if (!bridge.readAudioPeak) throw new Error('Unavailable');
        const value = await bridge.readAudioPeak(endpointId);
        if (!active) return;
        setPeak(Number.isFinite(value) ? Math.max(0, Math.min(1, value)) : 0);
        setFailed(false);
      } catch {
        if (!active) return;
        setPeak(0); setFailed(true); delay = 1000;
      }
      if (active) timer = setTimeout(sample, delay);
    }
    if (available) void sample();
    return () => { active = false; clearTimeout(timer); };
  }, [bridge, endpointId, available]);
  const percent = Math.round(peak * 100);
  return <div className={styles.container}>
    <div className={styles.header}><span>{failed || !available ? 'Уровень недоступен' : 'Уровень звука'}</span><span className={styles.value}>{percent}%</span></div>
    <div className={styles.track} role="meter" aria-label={`Уровень звука: ${label}`} aria-valuemin={0} aria-valuemax={100} aria-valuenow={percent}>
      <span className={styles.fill} style={{ width: `${percent}%` }} />
    </div>
  </div>;
}
