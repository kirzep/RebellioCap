import { expect, test } from '@playwright/test';
import { completeFirstRunFixture } from './fixtures/first-run-page';
test('recording toggle and replay save update state, settings opens', async ({ page }) => {
  await completeFirstRunFixture(page);
  const toggle = page.getByTestId('toggle-recording-button');
  await toggle.click();
  await expect(toggle).toHaveText('Остановить запись');
  await toggle.click();
  await expect(toggle).toHaveText('Начать запись');
  await page.getByTestId('save-replay-button').click();
  await expect.poll(() => page.evaluate(() => (window as unknown as { firstRunSnapshot: { metrics: { completedSaves: number } } }).firstRunSnapshot.metrics.completedSaves)).toBe(1);
  await page.getByRole('button', { name: 'Настройки', exact: true }).click();
  await expect(page.getByTestId('runtime-settings')).toBeVisible();
});

test('manual Replay memory budget uses app dropdown and persists after Apply', async ({page}) => {
  await completeFirstRunFixture(page);
  await page.getByRole('button', {name:'Настройки', exact:true}).click();
  await page.getByRole('radio', {name:'Повторы и файлы', exact:true}).click();
  const mode = page.getByLabel('Лимит памяти Replay', { exact: true });
  await mode.click();
  await expect(page.getByRole('option', {name:'Задать вручную'})).toBeVisible();
  await page.getByRole('option', {name:'Задать вручную'}).click();
  await page.getByLabel('Лимит, МиБ', { exact: true }).fill('2048');
  await page.screenshot({path:test.info().outputPath('replay-memory-limit.png')});
  await page.getByTestId('apply-settings-button').click();
  await expect.poll(() => page.evaluate(() => (window as any).firstRunState.active.replay_memory_limit_mb)).toBe(2048);
  await mode.click();
  await page.getByRole('option', {name:'Авто', exact:true}).click();
  await page.getByTestId('apply-settings-button').click();
  await expect.poll(() => page.evaluate(() => (window as any).firstRunState.active.replay_memory_limit_mb)).toBe(0);
});

// Retained while game-only capture is temporarily disabled.
test.skip('game-only settings preserve an explicit game when it closes and after reload', async ({ page }) => {
  await completeFirstRunFixture(page);
  await page.getByRole('button', { name: 'Настройки', exact: true }).click();
  const mode = page.getByLabel('Область захвата');
  await mode.click();
  await page.getByRole('option', { name: 'Только игра', exact: true }).click();
  await page.getByTestId('apply-settings-button').click();
  await expect(page.getByText('Выберите игру из списка.', { exact: true }).first()).toBeVisible();
  await page.getByLabel('Игра', { exact: true }).click();
  await page.getByRole('option', { name: /Example Game/ }).click();
  await page.evaluate(() => { (window as any).firstRunGameWindows = []; });
  await page.getByRole('button', { name: 'Обновить список игр', exact: true }).click();
  await expect(page.getByLabel('Игра', { exact: true })).toContainText('Ожидание игры');
  await expect(page.getByText(/Другие окна не попадают в видео/)).toBeVisible();
  await page.screenshot({ path: test.info().outputPath('game-only-settings.png') });
  await page.getByTestId('apply-settings-button').click();
  await expect.poll(() => page.evaluate(() => (window as any).firstRunState.active.capture_mode)).toBe('game');
  await expect.poll(() => page.evaluate(() => (window as any).firstRunState.active.game_executable)).toBe('C:\\Games\\Example\\game.exe');
  await page.reload();
  await page.getByRole('button', { name: 'Настройки', exact: true }).click();
  await expect(page.getByLabel('Область захвата')).toContainText('Только игра');
  await expect(page.getByLabel('Игра', { exact: true })).toContainText('Example Game');
});

test('capture settings offer only the full screen', async ({ page }) => {
  await completeFirstRunFixture(page);
  await page.getByRole('button', { name: 'Настройки', exact: true }).click();
  await expect(page.getByLabel('Область захвата')).toHaveCount(0);
  await expect(page.getByLabel('Игра', { exact: true })).toHaveCount(0);
  await expect(page.getByRole('combobox', { name: 'Экран', exact: true })).toBeVisible();
  await page.screenshot({ path: test.info().outputPath('fullscreen-only-settings.png') });
});

test('CQP and recording priority validate, persist and toggle independently', async ({ page }) => {
  await completeFirstRunFixture(page);
  await page.getByRole('button', { name: 'Настройки', exact: true }).click();
  await page.getByLabel('Режим сжатия', { exact: true }).click();
  await page.getByRole('option', { name: 'Постоянное качество (CQP)', exact: true }).click();
  await page.getByLabel('Уровень CQP', { exact: true }).fill('52');
  await page.getByTestId('apply-settings-button').click();
  await expect(page.getByText('Уровень CQP должен быть целым числом от 0 до 51.').first()).toBeVisible();
  await page.getByLabel('Уровень CQP', { exact: true }).fill('23');
  await page.getByRole('checkbox', { name: /Приоритет записи/ }).check();
  await expect(page.getByTestId('video-bitrate-input')).toHaveCount(0);
  await page.screenshot({ path: test.info().outputPath('cqp-recording-priority.png') });
  await page.getByTestId('apply-settings-button').click();
  await expect.poll(() => page.evaluate(() => (window as any).firstRunState.active.rate_control)).toBe('cqp');
  await expect.poll(() => page.evaluate(() => (window as any).firstRunState.active.cqp)).toBe(23);
  await expect.poll(() => page.evaluate(() => (window as any).firstRunState.active.recording_priority)).toBe(true);
  await page.reload();
  await page.getByRole('button', { name: 'Настройки', exact: true }).click();
  await expect(page.getByLabel('Уровень CQP', { exact: true })).toHaveValue('23');
  await expect(page.getByRole('checkbox', { name: /Приоритет записи/ })).toBeChecked();
  await page.getByRole('checkbox', { name: /Приоритет записи/ }).uncheck();
  await page.getByTestId('apply-settings-button').click();
  await expect.poll(() => page.evaluate(() => (window as any).firstRunState.active.recording_priority)).toBe(false);
  await expect.poll(() => page.evaluate(() => (window as any).firstRunState.active.rate_control)).toBe('cqp');
});
