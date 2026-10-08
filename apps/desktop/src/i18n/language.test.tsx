import { act, cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { invoke } from '@tauri-apps/api/core';
import { listen } from '@tauri-apps/api/event';
import { LanguageSettings } from '../components/LanguageSettings';
import { editorHistoryLabel } from './index';
import coreCatalog from './core.en.json';
import editorCatalog from './editor.en.json';
import nativeCatalog from './native.en.json';
import settingsCatalog from './settings.en.json';
import { applyLanguageSettings, disposeLanguageInitialization, getLanguageSnapshot, initializeLanguage,
  languageForLocale, setLanguagePreference, t, type LanguageSettings as Settings } from './index';

vi.mock('@tauri-apps/api/core', () => ({ invoke: vi.fn() }));
vi.mock('@tauri-apps/api/event', () => ({ listen: vi.fn() }));
beforeEach(() => { localStorage.clear(); vi.resetAllMocks(); });
afterEach(() => { cleanup(); disposeLanguageInitialization(); Reflect.deleteProperty(window, '__TAURI_INTERNALS__'); vi.restoreAllMocks(); });

describe('language selection', () => {
  it('keeps shared translations consistent and distinguishes editor actions from dialog actions', () => {
    const seen = new Map<string, string>();
    for (const catalog of [coreCatalog, editorCatalog, nativeCatalog, settingsCatalog]) {
      for (const [source, translated] of Object.entries(catalog)) {
        if (seen.has(source)) expect(translated, source).toBe(seen.get(source));
        seen.set(source, translated);
      }
    }
    applyLanguageSettings({ preference: 'en', language: 'en' });
    expect(t('Отменить')).toBe('Cancel');
    expect(t('Повторить')).toBe('Retry');
    expect(t('Отменить действие')).toBe('Undo');
    expect(t('Повторить действие')).toBe('Redo');
    expect(editorHistoryLabel('Отменить')).toBe('Undo');
    expect(editorHistoryLabel('Повторить')).toBe('Redo');
    applyLanguageSettings({ preference: 'ru', language: 'ru' });
    expect(editorHistoryLabel('Отменить')).toBe('Отменить');
    expect(editorHistoryLabel('Повторить')).toBe('Повторить');
  });
  it('uses Russian only for Russian Windows/browser language variants', () => {
    for (const language of ['ru', 'ru-RU', 'ru-BY', 'RU_ru']) expect(languageForLocale(language)).toBe('ru');
    for (const language of ['en-US', 'de-DE', 'uk-UA', 'fr', '', 'russian']) expect(languageForLocale(language)).toBe('en');
  });
  it('prefers the native Windows UI language over navigator language and initializes once', async () => {
    Object.defineProperty(window, '__TAURI_INTERNALS__', { value: {}, configurable: true });
    vi.mocked(listen).mockResolvedValue(() => {});
    vi.mocked(invoke).mockResolvedValue({ preference: 'system', language: 'ru' });
    const first = initializeLanguage();
    expect(initializeLanguage()).toBe(first);
    await first;
    expect(getLanguageSnapshot().language).toBe('ru');
    expect(document.documentElement.lang).toBe('ru');
    expect(invoke).toHaveBeenCalledOnce();
  });
  it('keeps a newer cross-window change when the initial read replies late', async () => {
    Object.defineProperty(window, '__TAURI_INTERNALS__', { value: {}, configurable: true });
    let receive!: (event: { payload: Settings }) => void;
    vi.mocked(listen).mockImplementation(async (_name, callback) => { receive = callback as typeof receive; return () => {}; });
    let finish!: (settings: Settings) => void;
    vi.mocked(invoke).mockImplementation(() => new Promise(resolve => { finish = resolve; }));
    const initializing = initializeLanguage();
    await waitFor(() => expect(finish).toBeDefined());
    receive({ payload: { preference: 'en', language: 'en' } });
    finish({ preference: 'system', language: 'ru' });
    await initializing;
    expect(getLanguageSnapshot()).toMatchObject({ preference: 'en', language: 'en' });
  });
  it('changes the mounted selector immediately, persists browser override and reloads it', async () => {
    render(<LanguageSettings />);
    fireEvent.keyDown(screen.getByRole('combobox', { name: 'Язык интерфейса' }), { key: 'ArrowDown' });
    fireEvent.click(screen.getByRole('option', { name: 'English' }));
    await screen.findByRole('combobox', { name: 'Interface language' });
    expect(localStorage.getItem('rebelliocap.language')).toBe('en');
    expect(document.documentElement.lang).toBe('en');
    applyLanguageSettings({ preference: 'system', language: 'ru' });
    await act(async () => { await initializeLanguage(); });
    expect(getLanguageSnapshot()).toMatchObject({ preference: 'en', language: 'en' });
  });
  it('retains the confirmed language when native persistence fails', async () => {
    Object.defineProperty(window, '__TAURI_INTERNALS__', { value: {}, configurable: true });
    vi.mocked(invoke).mockRejectedValue(new Error('write denied'));
    render(<LanguageSettings />);
    fireEvent.keyDown(screen.getByRole('combobox', { name: 'Язык интерфейса' }), { key: 'ArrowDown' });
    fireEvent.click(screen.getByRole('option', { name: 'English' }));
    await screen.findByRole('alert');
    expect(getLanguageSnapshot().language).toBe('ru');
    expect(invoke).toHaveBeenCalledWith('set_language_settings', { preference: 'en' });
  });
  it('rejects malformed responses without overwriting a valid preference', async () => {
    Object.defineProperty(window, '__TAURI_INTERNALS__', { value: {}, configurable: true });
    vi.mocked(invoke).mockResolvedValue({ preference: 'en', language: 'ru' });
    await expect(setLanguagePreference('en')).rejects.toThrow();
    expect(getLanguageSnapshot().language).toBe('ru');
  });
  it('translates parameterized errors without changing supplied user names', () => {
    applyLanguageSettings({ preference: 'en', language: 'en' });
    expect(t('Язык интерфейса')).toBe('Interface language');
    expect(t('Error: Язык интерфейса')).toBe('Error: Interface language');
    expect(t('Имя пользователя — Артём')).toBe('Имя пользователя — Артём');
  });
});
