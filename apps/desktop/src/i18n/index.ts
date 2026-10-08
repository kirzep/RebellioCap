import { useSyncExternalStore } from 'react';
import type { UnlistenFn } from '@tauri-apps/api/event';
import core from './core.en.json';
import editor from './editor.en.json';
import native from './native.en.json';
import settings from './settings.en.json';

export type Language = 'ru' | 'en';
export type LanguagePreference = 'system' | Language;
export interface LanguageSettings { preference: LanguagePreference; language: Language }
export interface LanguageSnapshot extends LanguageSettings { ready: boolean }
export const englishCatalog: Readonly<Record<string, string>> = { ...native, ...editor, ...core, ...settings };
const storageKey = 'rebelliocap.language';
let snapshot: LanguageSnapshot = { preference: 'system', language: 'ru', ready: false };
const subscribers = new Set<() => void>();
let initialization: Promise<void> | undefined;
let removeNativeListener: UnlistenFn | undefined;
let nativeEventRevision = 0;

export function languageForLocale(locale: string): Language {
  return /^ru(?:[-_]|$)/i.test(locale) ? 'ru' : 'en';
}
export function isLanguagePreference(value: unknown): value is LanguagePreference {
  return value === 'system' || value === 'ru' || value === 'en';
}
function validSettings(value: unknown): value is LanguageSettings {
  if (!value || typeof value !== 'object') return false;
  const record = value as Partial<LanguageSettings>;
  return isLanguagePreference(record.preference) && (record.language === 'ru' || record.language === 'en') &&
    (record.preference === 'system' || record.preference === record.language);
}
export function applyLanguageSettings(value: LanguageSettings): void {
  if (!validSettings(value)) throw new Error('Invalid language settings.');
  document.documentElement.lang = value.language;
  if (snapshot.ready && snapshot.language === value.language && snapshot.preference === value.preference) return;
  snapshot = { ...value, ready: true };
  subscribers.forEach(notify => notify());
}
function browserSettings(preference: LanguagePreference): LanguageSettings {
  return { preference, language: preference === 'system' ? languageForLocale(navigator.language) : preference };
}
function readBrowserPreference(): LanguagePreference {
  try {
    const value = localStorage.getItem(storageKey);
    return isLanguagePreference(value) ? value : 'system';
  } catch { return 'system'; }
}
export function initializeLanguage(): Promise<void> {
  if (initialization) return initialization;
  initialization = (async () => {
    if (!('__TAURI_INTERNALS__' in window)) {
      applyLanguageSettings(browserSettings(readBrowserPreference()));
      return;
    }
    try {
      const { listen } = await import('@tauri-apps/api/event');
      const { invoke } = await import('@tauri-apps/api/core');
      let eventRevision = 0;
      // Subscribe before reading so a concurrent change in another WebView is not missed.
      removeNativeListener = await listen<LanguageSettings>('language-changed', event => {
        if (validSettings(event.payload)) { eventRevision += 1; nativeEventRevision += 1; applyLanguageSettings(event.payload); }
      });
      const revisionBeforeRead = eventRevision;
      const value = await invoke<LanguageSettings>('get_language_settings');
      if (eventRevision === revisionBeforeRead) applyLanguageSettings(value);
    } catch (error) {
      console.warn('Could not initialize application language.', error);
      if (!snapshot.ready) applyLanguageSettings({ preference: 'system', language: 'en' });
    }
  })();
  return initialization;
}
window.addEventListener('storage', event => {
  if (event.key === storageKey && !('__TAURI_INTERNALS__' in window)) {
    applyLanguageSettings(browserSettings(readBrowserPreference()));
  }
});
export async function setLanguagePreference(preference: LanguagePreference): Promise<void> {
  if (!isLanguagePreference(preference)) throw new Error('Invalid language preference.');
  if ('__TAURI_INTERNALS__' in window) {
    const { invoke } = await import('@tauri-apps/api/core');
    const revisionBeforeWrite = nativeEventRevision;
    const value = await invoke<LanguageSettings>('set_language_settings', { preference });
    if (!validSettings(value) || value.preference !== preference) throw new Error('Invalid language response.');
    // A newer event from another WebView wins over this command's delayed reply.
    if (nativeEventRevision === revisionBeforeWrite || snapshot.preference === preference) applyLanguageSettings(value);
  } else {
    // Persist first: a failed write must leave the confirmed preference intact.
    localStorage.setItem(storageKey, preference);
    applyLanguageSettings(browserSettings(preference));
  }
}
const subscribe = (notify: () => void) => { subscribers.add(notify); return () => { subscribers.delete(notify); }; };
export function getLanguageSnapshot(): LanguageSnapshot { return snapshot; }
export function useLanguage(): LanguageSnapshot { return useSyncExternalStore(subscribe, getLanguageSnapshot); }
export function getLocale(): 'ru-RU' | 'en-US' { return snapshot.language === 'ru' ? 'ru-RU' : 'en-US'; }
function interpolate(source: string, values: readonly (string | number)[]): string {
  return source.replace(/\{(\d+)\}/g, (match, index: string) => values[Number(index)] === undefined ? match : String(values[Number(index)]));
}
const patterns = Object.entries(englishCatalog).flatMap(([source, translation]) => {
  const slots: number[] = [];
  const pieces = source.split(/(\{\d+\})/g);
  if (pieces.length === 1) return [];
  const regex = pieces.map(piece => {
    const slot = /^\{(\d+)\}$/.exec(piece);
    if (slot) { slots.push(Number(slot[1])); return '([\\s\\S]*?)'; }
    return piece.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  }).join('');
  return [{ regex: new RegExp(`^${regex}$`), slots, translation }];
});
export function t(source: string, ...values: (string | number)[]): string {
  if (snapshot.language === 'ru') return interpolate(source, values);
  const exact = englishCatalog[source];
  if (exact !== undefined) return interpolate(exact, values);
  if (source.startsWith('Error: ')) return `Error: ${t(source.slice(7), ...values)}`;
  // Native errors and persisted canonical UI messages may already contain their values.
  if (!values.length) {
    for (const pattern of patterns) {
      const match = pattern.regex.exec(source);
      if (!match) continue;
      const captured: string[] = [];
      pattern.slots.forEach((slot, index) => { captured[slot] = match[index + 1]; });
      return interpolate(pattern.translation, captured);
    }
  }
  return interpolate(source, values);
}
export function useTranslation() {
  const { language } = useLanguage();
  return { t, language, locale: getLocale() };
}
/** Release test listeners between isolated test/browser lifecycles. */
export function disposeLanguageInitialization(): void {
  removeNativeListener?.();
  removeNativeListener = undefined;
  initialization = undefined;
}
