import { beforeEach, expect, it, vi } from 'vitest';
import { emptyProject } from './model';
import { autosavePrefix, legacyAutosaveKey, readAutosaves, writeAutosave } from './autosave';

beforeEach(() => localStorage.clear());

it('keeps independent projects and updates only the same project', () => {
  const a = { id: 'a', savedAt: 1, project: { ...emptyProject(), name: 'A' } };
  const b = { id: 'b', savedAt: 2, project: { ...emptyProject(), name: 'B' } };
  writeAutosave(localStorage, a); writeAutosave(localStorage, b);
  writeAutosave(localStorage, { ...a, savedAt: 3, project: { ...a.project, name: 'A edited' } });
  expect(readAutosaves(localStorage).entries.map(e => [e.id, e.project.name])).toEqual([['a', 'A edited'], ['b', 'B']]);
});

it('isolates corrupt entries and still offers valid recovery', () => {
  localStorage.setItem(autosavePrefix + 'bad', '{');
  localStorage.setItem(autosavePrefix + 'invalid', JSON.stringify({ id: 'invalid', savedAt: 1, project: {} }));
  writeAutosave(localStorage, { id: 'good', savedAt: 2, project: emptyProject() });
  const result = readAutosaves(localStorage);
  expect(result.damaged).toBe(2);
  expect(result.entries.map(e => e.id)).toEqual(['good']);
  expect(localStorage.getItem(autosavePrefix + 'bad')).toBe('{');
});

it('reads legacy recovery and prefers its updated version without deleting the original', () => {
  const old = emptyProject();
  localStorage.setItem(legacyAutosaveKey, JSON.stringify(old));
  expect(readAutosaves(localStorage).entries[0].id).toBe('legacy-v1');
  writeAutosave(localStorage, { id: 'legacy-v1', savedAt: 10, project: { ...old, name: 'Updated' } });
  expect(readAutosaves(localStorage).entries).toHaveLength(1);
  expect(readAutosaves(localStorage).entries[0].project.name).toBe('Updated');
  expect(localStorage.getItem(legacyAutosaveKey)).toBe(JSON.stringify(old));
});

it('propagates quota failure and preserves previously stored content', () => {
  const entry = { id: 'a', savedAt: 1, project: emptyProject() };
  writeAutosave(localStorage, entry);
  const before = localStorage.getItem(autosavePrefix + 'a');
  const spy = vi.spyOn(Storage.prototype, 'setItem').mockImplementation(() => { throw new DOMException('full', 'QuotaExceededError'); });
  try { expect(() => writeAutosave(localStorage, { ...entry, savedAt: 2 })).toThrow(); }
  finally { spy.mockRestore(); }
  expect(localStorage.getItem(autosavePrefix + 'a')).toBe(before);
  expect(readAutosaves(localStorage).entries[0].savedAt).toBe(1);
});
