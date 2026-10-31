import { applyLanguageSettings } from '../i18n';
import { act, cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { WindowShell } from './WindowShell';
import type { UpdateProgress } from '../bridge/contracts';
import { UpdateNotice, localizedReleaseNotes } from './UpdateNotice';
import { SidebarProvider } from './ui/sidebar';

afterEach(cleanup);
function preview(onInstall = vi.fn().mockResolvedValue(undefined)) {
  render(<WindowShell navigation={[{ id: 'recording', label: 'Обзор' }]}
    settingsNavigation={{ id: 'settings', label: 'Настройки' }} onNavigate={vi.fn()}
    availableUpdate={{ version: '0.2.0', releaseNotes: 'Улучшения записи.' }} onInstallUpdate={onInstall}>
    <p>Обзор</p>
  </WindowShell>);
  return onInstall;
}
describe('update offer', () => {
  it('renders release Markdown instead of displaying its markers', () => {
    render(<SidebarProvider><UpdateNotice update={{ version: '0.2.0', releaseNotes: '***\n\n## Изменения\n\n- **Исправлена запись**\n- `Replay`\n\n<script>bad()</script>' }} onInstall={vi.fn()} /></SidebarProvider>);
    fireEvent.click(screen.getByRole('button', { name: 'Доступно обновление 0.2.0' }));
    expect(screen.getByRole('heading', { name: 'Изменения' })).toBeInTheDocument();
    expect(screen.getAllByRole('listitem').some(item => item.textContent === 'Исправлена запись')).toBe(true);
    expect(screen.getByText('Исправлена запись').tagName).toBe('STRONG');
    expect(screen.getByRole('dialog')).not.toHaveTextContent('***');
    expect(screen.getByRole('dialog').querySelector('script')).toBeNull();
    expect(screen.getByRole('dialog')).not.toHaveTextContent('bad()');
  });
  it('shows download progress and installation while preventing duplicate consent', async () => {
    let report: ((progress: UpdateProgress) => void) | undefined;
    let complete!: () => void;
    const pending = new Promise<void>(resolve => { complete = resolve; });
    render(<WindowShell navigation={[{ id: 'recording', label: 'Обзор' }]} onNavigate={vi.fn()}
      settingsNavigation={{ id: 'settings', label: 'Настройки' }}
      availableUpdate={{ version: '0.2.0' }} onInstallUpdate={async onProgress => {
      report = onProgress;
      await pending;
    }}><p>Обзор</p></WindowShell>);
    fireEvent.click(screen.getByRole('button', { name: 'Доступно обновление 0.2.0' }));
    fireEvent.click(screen.getByRole('button', { name: 'Обновить и перезапустить' }));
    await waitFor(() => expect(report).toBeDefined());
    const { act } = await import('@testing-library/react');
    act(() => report!({ phase: 'downloading', downloadedBytes: 50, totalBytes: 100 }));
    expect(screen.getByRole('progressbar')).toHaveAttribute('value', '50');
    expect(screen.getByRole('button', { name: 'Позже' })).toBeDisabled();
    act(() => report!({ phase: 'installing', downloadedBytes: 100, totalBytes: 100 }));
    expect(screen.getByRole('status')).toHaveTextContent('Устанавливаем');
    complete();
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
  });
  it('requires confirmation and allows postponing without interrupting recording', () => {
    const install = preview();
    fireEvent.click(screen.getByRole('button', { name: 'Доступно обновление 0.2.0' }));
    expect(screen.getByRole('dialog')).toHaveTextContent('Текущая запись остановится');
    expect(screen.getByRole('dialog')).toHaveTextContent('приложение перезапустится');
    expect(install).not.toHaveBeenCalled();
    fireEvent.click(screen.getByRole('button', { name: 'Позже' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(install).not.toHaveBeenCalled();
  });
  it('starts installation once after explicit confirmation', async () => {
    const install = preview();
    fireEvent.click(screen.getByRole('button', { name: 'Доступно обновление 0.2.0' }));
    fireEvent.click(screen.getByRole('button', { name: 'Обновить и перезапустить' }));
    await waitFor(() => expect(install).toHaveBeenCalledOnce());
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
  });
  it('shows an installation error and keeps the offer available for retry', async () => {
    const install = preview(vi.fn().mockRejectedValue(new Error('Ошибка загрузки')));
    fireEvent.click(screen.getByRole('button', { name: 'Доступно обновление 0.2.0' }));
    fireEvent.click(screen.getByRole('button', { name: 'Обновить и перезапустить' }));
    await screen.findByText('Не удалось обновить приложение');
    expect(screen.getByRole('dialog')).toBeInTheDocument();
    expect(install).toHaveBeenCalledOnce();
  });
});

it('selects release notes by language and preserves legacy notes', () => {
 const notes = '## Русский\n\n- Исправлен повтор.\n\n## English\n\n- Fix replay.';
 expect(localizedReleaseNotes(notes, 'ru')).toBe('- Исправлен повтор.');
 expect(localizedReleaseNotes(notes, 'en')).toBe('- Fix replay.');
 expect(localizedReleaseNotes('- Legacy notes.', 'en')).toBe('- Legacy notes.');
 expect(localizedReleaseNotes('## Русский\n- Only Russian.', 'en')).toBe('- Only Russian.');
});

it('updates the visible notes immediately when the application language changes', () => {
 try {
  render(<SidebarProvider><UpdateNotice update={{version:'0.1.25',releaseNotes:'## Русский\n- Исправлен повтор.\n\n## English\n- Fix replay.'}} onInstall={vi.fn()}/></SidebarProvider>);
  fireEvent.click(screen.getByRole('button', {name:'Доступно обновление 0.1.25'}));
  expect(screen.getByRole('dialog')).toHaveTextContent('Исправлен повтор.');
  expect(screen.getByRole('dialog')).not.toHaveTextContent('Fix replay.');
  act(()=>applyLanguageSettings({preference:'en',language:'en'}));
  expect(screen.getByRole('dialog')).toHaveTextContent('Fix replay.');
  expect(screen.getByRole('dialog')).not.toHaveTextContent('Исправлен повтор.');
 } finally { act(()=>applyLanguageSettings({preference:'ru',language:'ru'})); }
});
