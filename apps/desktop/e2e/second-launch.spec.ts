import { expect, test } from '@playwright/test';
import { completeFirstRunFixture } from './fixtures/first-run-page';
test('completed onboarding survives reload and opens home directly', async ({ page }) => {
  await completeFirstRunFixture(page);
  await page.reload();
  await expect(page.getByTestId('home-panel')).toBeVisible();
  await expect(page.getByTestId('start-screen')).toHaveCount(0);
  await expect(page.getByTestId('onboarding-panel')).toHaveCount(0);
});