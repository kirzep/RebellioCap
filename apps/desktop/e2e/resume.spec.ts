import { expect, test } from '@playwright/test';
import { openFirstRunFixture } from './fixtures/first-run-page';

test.describe('Onboarding Resume Flow', () => {
  test('navigates through steps and verifies progress tracking and back navigation', async ({
    page,
  }) => {
    await openFirstRunFixture(page);

    await expect(page.getByTestId('start-screen')).toBeVisible();
    await page.getByTestId('start-button').click();

    await expect(page.getByTestId('system-check-screen')).toBeVisible();
    const continueButton = page.getByTestId('check-continue-button');
    await expect(continueButton).toBeEnabled();
    await continueButton.click();

    // Step 1 -> Step 2
    await expect(page.getByTestId('step-counter')).toHaveText('Шаг 1 из 6');
    await page.getByTestId('onboarding-next-button').click();

    // In Step 2, click back button
    await expect(page.getByTestId('step-counter')).toHaveText('Шаг 2 из 6');
    await page.reload();
    await expect(page.getByTestId('step-counter')).toHaveText('Шаг 2 из 6');
    await page.getByTestId('onboarding-back-button').click();

    // Returned to Step 1
    await expect(page.getByTestId('step-counter')).toHaveText('Шаг 1 из 6');
    await expect(page.getByTestId('video-step')).toBeVisible();
  });
});
