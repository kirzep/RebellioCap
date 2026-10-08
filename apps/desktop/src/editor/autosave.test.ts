import { beforeEach, expect, it, vi } from 'vitest';
import { emptyProject, newItem } from './model';
import { autosavePrefix, legacyAutosaveKey, readAutosaves, writeAutosave, dismissAutosaves } from './autosave';

function contentProject() { return {...emptyProject(), items:[newItem('text',0,0,5)]}; }

beforeEach(() => localStorage.clear());

it('keeps independent projects and updates only the same project', () => {
  const a = { id: 'a', savedAt: 1, project: { ...contentProject(), name: 'A' } };
  const b = { id: 'b', savedAt: 2, project: { ...contentProject(), name: 'B' } };
  writeAutosave(localStorage, a); writeAutosave(localStorage, b);
  writeAutosave(localStorage, { ...a, savedAt: 3, project: { ...a.project, name: 'A edited' } });
  expect(readAutosaves(localStorage).entries.map(e => [e.id, e.project.name])).toEqual([['a', 'A edited'], ['b', 'B']]);
});

it('isolates corrupt entries and still offers valid recovery', () => {
  localStorage.setItem(autosavePrefix + 'bad', '{');
  localStorage.setItem(autosavePrefix + 'invalid', JSON.stringify({ id: 'invalid', savedAt: 1, project: {} }));
  writeAutosave(localStorage, { id: 'good', savedAt: 2, project: contentProject() });
  const result = readAutosaves(localStorage);
  expect(result.damaged).toBe(2);
  expect(result.entries.map(e => e.id)).toEqual(['good']);
  expect(localStorage.getItem(autosavePrefix + 'bad')).toBe('{');
});

it('reads legacy recovery and prefers its updated version without deleting the original', () => {
  const old = contentProject();
  localStorage.setItem(legacyAutosaveKey, JSON.stringify(old));
  expect(readAutosaves(localStorage).entries[0].id).toBe('legacy-v1');
  writeAutosave(localStorage, { id: 'legacy-v1', savedAt: 10, project: { ...old, name: 'Updated' } });
  expect(readAutosaves(localStorage).entries).toHaveLength(1);
  expect(readAutosaves(localStorage).entries[0].project.name).toBe('Updated');
  expect(localStorage.getItem(legacyAutosaveKey)).toBe(JSON.stringify(old));
});

it('propagates quota failure and preserves previously stored content', () => {
  const entry = { id: 'a', savedAt: 1, project: contentProject() };
  writeAutosave(localStorage, entry);
  const before = localStorage.getItem(autosavePrefix + 'a');
  const spy = vi.spyOn(Storage.prototype, 'setItem').mockImplementation(() => { throw new DOMException('full', 'QuotaExceededError'); });
  try { expect(() => writeAutosave(localStorage, { ...entry, savedAt: 2 })).toThrow(); }
  finally { spy.mockRestore(); }
  expect(localStorage.getItem(autosavePrefix + 'a')).toBe(before);
  expect(readAutosaves(localStorage).entries[0].savedAt).toBe(1);
});

it('makes a newly edited snapshot eligible after declining its previous recovery', () => {
 const entry={id:'a',savedAt:1,project:contentProject()};
 writeAutosave(localStorage,entry);dismissAutosaves(localStorage,[entry]);
 expect(readAutosaves(localStorage).entries).toEqual([]);
 writeAutosave(localStorage,{...entry,savedAt:2});
 expect(readAutosaves(localStorage).entries[0].savedAt).toBe(2);
});
it('dismisses a legacy recovery without offering it again', () => {
 localStorage.setItem(legacyAutosaveKey,JSON.stringify(contentProject()));
 dismissAutosaves(localStorage,readAutosaves(localStorage).entries);
 expect(readAutosaves(localStorage).entries).toEqual([]);
 expect(localStorage.getItem(autosavePrefix+'legacy-v1')).not.toBeNull();
});
it('does not create a recovery for an empty renamed project', () => {
 writeAutosave(localStorage,{id:'empty',savedAt:1,project:{...emptyProject(),name:'Renamed'}});
 expect(localStorage.getItem(autosavePrefix+'empty')).toBeNull();
});
