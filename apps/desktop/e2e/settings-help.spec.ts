import { expect, test } from '@playwright/test';
import { completeFirstRunFixture } from './fixtures/first-run-page';

test('settings help opens on hover, stays readable and closes with Escape', async ({ page }) => {
  await completeFirstRunFixture(page);
  await page.getByRole('button', { name: 'Настройки', exact: true }).click();
  const help = page.getByRole('button', { name: 'Подсказка: Режим сжатия', exact: true });
  await help.hover();
  const tooltip = page.getByRole('tooltip');
  await expect(tooltip).toContainText('VBR');
  await expect(tooltip).toContainText('CQP');
  await tooltip.hover();
  await expect(tooltip).toBeVisible();
  await page.screenshot({ animations: 'disabled', path: test.info().outputPath('settings-help-ru.png') });
  await page.keyboard.press('Escape');
  await expect(tooltip).toHaveCount(0);
  await expect(page.getByTestId('runtime-settings')).toBeVisible();
});

test('keyboard help follows language changes and leaves settings untouched', async ({ page }) => {
  await completeFirstRunFixture(page);
  await page.getByRole('button', { name: 'Настройки', exact: true }).click();
  const help = page.getByRole('button', { name: 'Подсказка: Режим сжатия', exact: true });
  await help.focus();
  await expect(page.getByRole('tooltip')).toContainText('VBR');
  await page.evaluate(async () => {
    const { setLanguagePreference } = await import('/src/i18n/index.ts');
    await setLanguagePreference('en');
  });
  await expect(page.getByRole('tooltip')).toContainText('file size');
  await expect(page.getByRole('button', { name: 'Help: Compression mode', exact: true })).toBeFocused();
  await page.screenshot({ animations: 'disabled', path: test.info().outputPath('settings-help-en.png') });
  await page.keyboard.press('Escape');
  await expect(page.getByRole('tooltip')).toHaveCount(0);
  await page.getByRole('radio', { name: 'Replays and files', exact: true }).click();
  await page.getByRole('button', { name: 'Help: Replay memory limit', exact: true }).focus();
  await expect(page.getByRole('tooltip')).toContainText('shorter');
  await page.keyboard.press('Escape');
  await expect(page.getByTestId('apply-settings-button')).toBeDisabled();
});

test('help beside a checkbox does not change the checkbox', async ({ page }) => {
  await completeFirstRunFixture(page);
  await page.getByRole('button', { name: 'Настройки', exact: true }).click();
  const checkbox = page.getByRole('checkbox', { name: /Приоритет записи/ });
  await expect(checkbox).not.toBeChecked();
  const help = page.getByRole('button', { name: 'Подсказка: Приоритет записи', exact: true });
  await help.focus();
  await expect(page.getByRole('tooltip')).toContainText('игре');
  await help.click();
  await expect(checkbox).not.toBeChecked();
  await expect(page.getByTestId('apply-settings-button')).toBeDisabled();
});
