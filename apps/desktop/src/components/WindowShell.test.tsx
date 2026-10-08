import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, expect, it } from 'vitest';
import { WindowShell } from './WindowShell';
import { applyLanguageSettings, disposeLanguageInitialization, initializeLanguage } from '../i18n';

beforeEach(() => { localStorage.clear(); });
afterEach(() => { cleanup(); localStorage.clear(); });

it('automatically collapses the editor sidebar and restores the previous choice', () => {
  applyLanguageSettings({preference:'ru',language:'ru'});
  const props={navigation:[{id:'editor',label:'Редактор клипов'}],onNavigate:()=>{}};
  const view=render(<WindowShell {...props} activeNavigation="recording">Workspace</WindowShell>);
  view.rerender(<WindowShell {...props} activeNavigation="editor">Editor</WindowShell>);
  expect(screen.getByRole('button',{name:'Развернуть боковое меню'})).toHaveAttribute('aria-expanded','false');
  expect(localStorage.getItem('rebelliocap.sidebar-collapsed')).toBeNull();
  view.rerender(<WindowShell {...props} activeNavigation="clips">Clips</WindowShell>);
  expect(screen.getByRole('button',{name:'Свернуть боковое меню'})).toHaveAttribute('aria-expanded','true');
});

it('keeps language above Settings and usable in the collapsed sidebar without opening settings', async () => {
  render(<WindowShell navigation={[{ id: 'recording', label: 'Запись' }]}
    settingsNavigation={{ id: 'settings', label: 'Настройки' }} onNavigate={() => {}}>
    <h1>Workspace</h1>
  </WindowShell>);
  const language = screen.getByRole('combobox', { name: 'Язык интерфейса' });
  const settings = screen.getByRole('button', { name: 'Настройки' });
  expect(language.compareDocumentPosition(settings) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
  fireEvent.click(screen.getByRole('button', { name: 'Свернуть боковое меню' }));
  expect(language).toBeEnabled();
  fireEvent.keyDown(language, { key: 'ArrowDown' });
  fireEvent.click(screen.getByRole('option', { name: 'English' }));
  await waitFor(() => expect(screen.getByRole('combobox', { name: 'Interface language' })).toHaveTextContent('English'));
  expect(localStorage.getItem('rebelliocap.language')).toBe('en');
  cleanup();
  disposeLanguageInitialization();
  applyLanguageSettings({ preference: 'system', language: 'ru' });
  await initializeLanguage();
  render(<WindowShell navigation={[{ id: 'recording', label: 'Запись' }]}
    settingsNavigation={{ id: 'settings', label: 'Настройки' }} onNavigate={() => {}}>Workspace</WindowShell>);
  expect(screen.getByRole('combobox', { name: 'Interface language' })).toHaveTextContent('English');
  expect(screen.getByRole('button', { name: 'Expand sidebar' })).toHaveAttribute('aria-expanded', 'false');
});
