import { expect, test } from '@playwright/test';
import { completeFirstRunFixture } from './fixtures/first-run-page';

test('video size preserves encoding and FPS, restores custom dimensions and translates help', async ({ page }) => {
  await completeFirstRunFixture(page);
  await page.getByRole('button', { name: 'Настройки', exact: true }).click();
  await expect(page.getByRole('radio', { name: 'Сбалансированное', exact: true })).toHaveCount(0);
  await expect(page.getByRole('radio', { name: 'Как у экрана', exact: true })).toBeChecked();
  await page.getByRole('combobox', { name: 'Частота кадров', exact: true }).click();
  await page.getByRole('option', { name: '120 кадров/с', exact: true }).click();
  await page.getByLabel('Битрейт, Мбит/с', { exact: true }).fill('9');
  await page.getByRole('combobox', { name: 'Режим сжатия', exact: true }).click();
  await page.getByRole('option', { name: 'Постоянное качество (CQP)', exact: true }).click();
  await page.getByLabel('Уровень CQP', { exact: true }).fill('21');
  await page.getByRole('radio', { name: 'Свой размер', exact: true }).click();
  await page.getByLabel('Ширина', { exact: true }).fill('1600');
  await page.getByLabel('Высота', { exact: true }).fill('900');
  await page.getByRole('radio', { name: '1920×1080', exact: true }).click();
  await expect(page.getByRole('radio', { name: '1920×1080', exact: true })).toBeChecked();
  await expect(page.getByLabel('Ширина', { exact: true })).toHaveCount(0);
  await page.getByTestId('apply-settings-button').click();
  await expect.poll(() => page.evaluate(() => {
    const { width, height, fps, bitrate, rate_control, cqp } = (window as any).firstRunState.active;
    return { width, height, fps, bitrate, rate_control, cqp };
  })).toEqual({ width: 1920, height: 1080, fps: 120, bitrate: 9_000_000, rate_control: 'cqp', cqp: 21 });
  await page.getByRole('radio', { name: 'Свой размер', exact: true }).click();
  await page.getByLabel('Ширина', { exact: true }).fill('1280');
  await page.getByLabel('Высота', { exact: true }).fill('720');
  await page.getByTestId('apply-settings-button').click();
  await expect(page.getByTestId('apply-settings-button')).toBeDisabled();
  await page.getByRole('radio', { name: 'Звук', exact: true }).click();
  await page.getByRole('radio', { name: 'Экран', exact: true }).click();
  await expect(page.getByRole('radio', { name: 'Свой размер', exact: true })).toBeChecked();
  await expect(page.getByLabel('Ширина', { exact: true })).toHaveValue('1280');
  await expect(page.getByLabel('Высота', { exact: true })).toHaveValue('720');
  await page.screenshot({ animations: 'disabled', path: test.info().outputPath('video-size-ru.png') });
  await page.evaluate(async () => {
    const { setLanguagePreference } = await import('/src/i18n/index.ts');
    await setLanguagePreference('en');
  });
  await expect(page.getByRole('radiogroup', { name: 'Video size', exact: true })).toBeVisible();
  await expect(page.getByRole('radio', { name: 'Custom size', exact: true })).toBeChecked();
  await page.getByRole('button', { name: 'Help: Video size', exact: true }).hover();
  await expect(page.getByRole('tooltip')).toContainText('Frame rate and compression are set separately.');
  await page.screenshot({ animations: 'disabled', path: test.info().outputPath('video-size-en.png') });
});
