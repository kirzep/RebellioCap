import { expect, test } from '@playwright/test';
import { openFirstRunFixture } from './fixtures/first-run-page';

test.use({ locale: 'de-DE' });
test('non-Russian system language defaults to English and overrides survive reload', async ({ page }) => {
  await openFirstRunFixture(page);
  await expect(page.locator('html')).toHaveAttribute('lang', 'en');
  await page.getByTestId('start-button').click();
  await page.getByTestId('check-continue-button').click();
  for (let step = 1; step <= 5; step++) {
    await expect(page.getByTestId('step-counter')).toHaveText(`Step ${step} of 6`);
    if (step === 3) await page.getByTestId('choose-folder-button').click();
    await page.getByTestId('onboarding-next-button').click();
  }
  await page.getByTestId('run-test-button').click();
  await page.getByTestId('complete-onboarding-button').click();
  const language = page.getByRole('combobox', { name: 'Interface language' });
  await expect(language).toHaveValue('system');
  const languageBounds = await language.boundingBox();
  const settingsBounds = await page.getByRole('button', { name: 'Settings', exact: true }).boundingBox();
  expect(languageBounds!.y + languageBounds!.height).toBeLessThanOrEqual(settingsBounds!.y);
  await expect(page.getByRole('radio', { name: 'Application', exact: true })).toHaveCount(0);
  await page.getByLabel('Interface language').selectOption('ru');
  await expect(page.locator('html')).toHaveAttribute('lang', 'ru');
  await expect(page.getByLabel('Язык интерфейса')).toHaveValue('ru');
  await page.reload();
  await expect(page.locator('html')).toHaveAttribute('lang', 'ru');
  await expect(page.getByLabel('Язык интерфейса')).toHaveValue('ru');
  await page.getByRole('button', { name: 'Свернуть боковое меню', exact: true }).click();
  await expect(page.getByLabel('Язык интерфейса')).toBeVisible();
  await page.getByLabel('Язык интерфейса').focus();
  await expect(page.getByLabel('Язык интерфейса')).toBeFocused();
  const collapsedBounds = await page.getByLabel('Язык интерфейса').boundingBox();
  expect(collapsedBounds!.width).toBeLessThan(languageBounds!.width);
  await page.getByLabel('Язык интерфейса').selectOption('en');
  await expect(page.getByLabel('Interface language')).toHaveValue('en');
  await page.reload();
  await expect(page.getByLabel('Interface language')).toHaveValue('en');
  await expect(page.getByRole('button', { name: 'Expand sidebar', exact: true })).toHaveAttribute('aria-expanded', 'false');
  await page.getByLabel('Interface language').selectOption('system');
  await expect(page.locator('html')).toHaveAttribute('lang', 'en');
  await page.reload();
  await expect(page.getByLabel('Interface language')).toHaveValue('system');
  await expect(page.locator('html')).toHaveAttribute('lang', 'en');
  await page.getByRole('button', { name: 'Settings', exact: true }).click();
  await page.getByRole('radio', { name: 'Application', exact: true }).click();
  await expect(page.getByRole('combobox', { name: 'Interface language' })).toHaveCount(1);
});
