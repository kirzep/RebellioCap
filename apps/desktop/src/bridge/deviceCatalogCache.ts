import type { HostBridge } from './contracts';
import type { AudioCatalog, MonitorChoice } from '../config/model';

// Keep the last successful catalog for immediate navigation; each screen still
// refreshes it from the host so unplugged devices and new devices are detected.
export const monitorCatalogCache = new WeakMap<HostBridge, MonitorChoice[]>();
export const audioCatalogCache = new WeakMap<HostBridge, AudioCatalog>();
