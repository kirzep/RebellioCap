import type { Project } from './model';

export const autosavePrefix = 'rebcap.editor.autosave.v2.';
export const legacyAutosaveKey = 'rebcap.editor.autosave.v1';
export type Recovery = { id: string; savedAt: number; project: Project };
function object(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}
function finite(value: unknown): value is number { return typeof value === 'number' && Number.isFinite(value); }
function validProject(value: unknown): value is Project {
  if (!object(value) || value.version !== 1 || typeof value.name !== 'string' ||
      !finite(value.width) || value.width < 16 || value.width > 7680 ||
      !finite(value.height) || value.height < 16 || value.height > 7680 ||
      !finite(value.fps) || value.fps < 1 || value.fps > 240 ||
      !Array.isArray(value.assets) || value.assets.length > 200 ||
      !Array.isArray(value.items) || value.items.length > 500) return false;
  const assets = value.assets.every(a => object(a) && typeof a.id === 'string' &&
    typeof a.path === 'string' && typeof a.name === 'string' && ['video', 'audio', 'image'].includes(String(a.kind)) &&
    finite(a.duration) && a.duration >= 0 && finite(a.width) && finite(a.height) &&
    Array.isArray(a.audio) && a.audio.every(finite));
  const items = value.items.every(i => object(i) && typeof i.id === 'string' &&
    ['video', 'audio', 'image', 'text'].includes(String(i.kind)) &&
    ['track','start','duration','source','stream','x','y','width','height','opacity','gain','brightness','contrast','saturation','fontSize'].every(k => finite(i[k])) &&
    Number(i.start) >= 0 && Number(i.source) >= 0 && Number(i.duration) >= .01 &&
    typeof i.text === 'string' && typeof i.color === 'string' && ['contain','cover'].includes(String(i.fit)));
  return assets && items;
}
export function validRecoveryId(id: unknown): id is string {
  return typeof id === 'string' && /^[a-zA-Z0-9-]{1,128}$/.test(id);
}
export function readAutosaves(storage: Storage): { entries: Recovery[]; damaged: number } {
  const entries: Recovery[] = [];
  let damaged = 0;
  const keys: string[] = [];
  for (let index = 0; index < storage.length; index++) {
    const key = storage.key(index);
    if (key?.startsWith(autosavePrefix)) keys.push(key);
  }
  for (const key of keys) {
    const raw = storage.getItem(key);
    if (raw === null) continue;
    try {
      const entry: unknown = JSON.parse(raw);
      if (!object(entry) || entry.version !== 2 || !validRecoveryId(entry.id) ||
          key !== autosavePrefix + entry.id || !finite(entry.savedAt) || entry.savedAt < 0 || !validProject(entry.project)) throw Error('Invalid autosave');
      entries.push({ id: entry.id, savedAt: entry.savedAt, project: entry.project });
    } catch { damaged++; }
  }
  const raw = storage.getItem(legacyAutosaveKey);
  if (raw !== null && !entries.some(e => e.id === 'legacy-v1')) {
    try {
      const project: unknown = JSON.parse(raw);
      if (!validProject(project)) throw Error('Invalid legacy autosave');
      entries.push({ id: 'legacy-v1', savedAt: 0, project });
    } catch { damaged++; }
  }
  entries.sort((a, b) => b.savedAt - a.savedAt || a.id.localeCompare(b.id));
  return { entries, damaged };
}
export function writeAutosave(storage: Storage, entry: Recovery) {
  if (!validRecoveryId(entry.id) || !finite(entry.savedAt) || entry.savedAt < 0 || !validProject(entry.project)) throw Error('Некорректная автокопия проекта');
  storage.setItem(autosavePrefix + entry.id, JSON.stringify({ version: 2, ...entry }));
}
