import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { WindowShell } from './WindowShell';

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
