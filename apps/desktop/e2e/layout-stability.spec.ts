import { expect, test } from '@playwright/test';
import { completeFirstRunFixture } from './fixtures/first-run-page';

test('capture controls keep their positions through recording and replay feedback', async ({ page }, testInfo) => {
  await completeFirstRunFixture(page);
  for (const width of [1120, 800]) {
    await page.setViewportSize({ width, height: 900 });
    await page.evaluate(async () => {
      await new Promise(requestAnimationFrame);
      await Promise.all(document.getAnimations().filter(animation => animation.effect?.getTiming().iterations !== Infinity).map(animation => animation.finished.catch(() => {})));
    });
    const controls = page.getByTestId('recording-controls');
    const save = page.getByTestId('save-replay-button');
    const toggle = page.getByTestId('toggle-recording-button');
    const positions = async () => Promise.all([controls, save, toggle].map(async locator => {
      return locator.evaluate(element => {
        const box = element.getBoundingClientRect();
        const main = element.closest('main');
        return { x: box.x, y: box.y + (main?.scrollTop ?? 0), width: box.width, height: box.height };
      });
    }));
    const baseline = await positions();
    await toggle.click();
    await expect(toggle).toBeEnabled();
    expect(await positions()).toEqual(baseline);
    await page.screenshot({ path: testInfo.outputPath(`stable-${width}.png`) });
    await toggle.click();
    await expect(toggle).toBeEnabled();
    expect(await positions()).toEqual(baseline);
    await save.click();
    await expect(page.getByText('Повтор сохранён.', { exact: true })).toBeVisible();
    expect(await positions()).toEqual(baseline);
    await expect(page.getByText('Повтор сохранён.', { exact: true })).toHaveCount(0, { timeout: 10000 });
    expect(await positions()).toEqual(baseline);
  }
});

test('settings actions stay at the bottom right without a divider while scrolling or editing', async ({ page }, testInfo) => {
  await page.setViewportSize({ width: 1120, height: 900 });
  await completeFirstRunFixture(page);
  await page.getByRole('button', { name: 'Настройки', exact: true }).click();
  const settings = page.getByTestId('runtime-settings');
  await settings.getByRole('radio', { name: 'Экран', exact: true }).click();
  const apply = page.getByTestId('apply-settings-button');
  await apply.scrollIntoViewIfNeeded();
  const position = () => apply.evaluate(element => element.getBoundingClientRect().y);
  const baseline = await position();
  const host = page.getByTestId('workspace-actions');
  const footer = host.locator('> div');
  expect(await footer.evaluate(element => getComputedStyle(element).borderTopWidth)).toBe('0px');
  const applyBox = await apply.boundingBox();
  expect(1120 - (applyBox!.x + applyBox!.width)).toBeLessThanOrEqual(50);
  expect(900 - (applyBox!.y + applyBox!.height)).toBeLessThanOrEqual(50);
  await page.getByRole('main').evaluate(element => { element.scrollTop = 0; });
  expect(await position()).toBe(baseline);
  await page.getByRole('main').evaluate(element => { element.scrollTop = element.scrollHeight; });
  expect(await position()).toBe(baseline);
  await settings.getByLabel('Ширина', { exact: true }).fill('1280');
  await settings.getByLabel('Ширина', { exact: true }).blur();
  await expect(apply).toBeEnabled();
  expect(await position()).toBe(baseline);
  await page.getByTestId('cancel-settings-button').click();
  await expect(apply).toBeDisabled();
  expect(await position()).toBe(baseline);
  await page.screenshot({ path: testInfo.outputPath('compact-settings.png') });
  for (const section of ['Звук', 'Повторы и файлы', 'Горячие клавиши']) {
    await settings.getByRole('radio', { name: section, exact: true }).click();
    await apply.scrollIntoViewIfNeeded();
    expect(await position()).toBe(baseline);
  }
});
