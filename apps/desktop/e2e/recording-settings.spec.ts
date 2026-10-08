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
  const mode = page.getByLabel('Лимит памяти Replay');
  await mode.click();
  await expect(page.getByRole('option', {name:'Задать вручную'})).toBeVisible();
  await page.getByRole('option', {name:'Задать вручную'}).click();
  await page.getByLabel('Лимит, МиБ').fill('2048');
  await page.screenshot({path:test.info().outputPath('replay-memory-limit.png')});
  await page.getByTestId('apply-settings-button').click();
  await expect.poll(() => page.evaluate(() => (window as any).firstRunState.active.replay_memory_limit_mb)).toBe(2048);
  await mode.click();
  await page.getByRole('option', {name:'Авто', exact:true}).click();
  await page.getByTestId('apply-settings-button').click();
  await expect.poll(() => page.evaluate(() => (window as any).firstRunState.active.replay_memory_limit_mb)).toBe(0);
});
