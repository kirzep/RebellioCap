import { expect, test } from '@playwright/test';
import { openFirstRunFixture } from './fixtures/first-run-page';

test.describe('Accessibility and Keyboard Navigation', () => {
  test('supports full keyboard navigation with visible focus indicators and accessible labels', async ({
    page,
  }) => {
    await openFirstRunFixture(page);

    // Start Screen has accessible button
    const startBtn = page.getByTestId('start-button');
    await expect(startBtn).toBeVisible();
    await expect(startBtn).toHaveAttribute('type', 'button');

    // The brand button precedes the primary action in keyboard order.
    await page.keyboard.press('Tab');
    await expect(page.getByRole('button', { name: 'RebellioCap', exact: true })).toBeFocused();
    await page.keyboard.press('Tab');
    await expect(startBtn).toBeFocused();
    expect(await startBtn.evaluate(element => element.matches(':focus-visible'))).toBe(true);

    // Press Enter to activate
    await page.keyboard.press('Enter');

    // System Check Screen
    await expect(page.getByTestId('system-check-screen')).toBeVisible();
    const continueBtn = page.getByTestId('check-continue-button');
    await expect(continueBtn).toBeEnabled();

    // Visible text supplies the accessible name of the back button.
    const backBtn = page.getByTestId('shell-back-button');
    await expect(backBtn).toHaveAccessibleName('Назад');

    // Verify progress navigation has aria-label
    await continueBtn.click();
    await expect(page.getByTestId('onboarding-panel')).toBeVisible();
    const nav = page.getByRole('navigation', { name: 'Шаги настройки' });
    await expect(nav).toBeVisible();
  });
});
