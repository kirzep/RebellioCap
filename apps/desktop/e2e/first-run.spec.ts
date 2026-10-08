import { expect, test } from '@playwright/test';

if (process.env.PLAYWRIGHT_CHANNEL) test.use({ channel: process.env.PLAYWRIGHT_CHANNEL });

test.describe('First Run Flow', () => {
  test('reports an unavailable desktop host in a plain browser', async ({ page }) => {
    await page.goto('/');
    await expect(page.getByText('Не удалось связаться с desktop-службой RebellioCap.', { exact: true })).toBeVisible();
    await expect(page.getByTestId('start-screen')).toHaveCount(0);
  });

  test('guides new user through start, system check, 6 onboarding steps, test, and arrives at home', async ({
    page,
  }) => {
    await page.route('**/first-run-qa', route => route.fulfill({
      contentType: 'text/html',
      body: `<!doctype html><html><body><div id="root"></div>
        <script type="module">import RefreshRuntime from '/@react-refresh';
        RefreshRuntime.injectIntoGlobalHook(window);window.$RefreshReg$=()=>{};
        window.$RefreshSig$=()=>type=>type;window.__vite_plugin_react_preamble_installed__=true;</script>
        <script type="module" src="/e2e/fixtures/first-run.tsx"></script></body></html>`,
    }));
    await page.goto('/first-run-qa');

    // 1. Initial screen: StartScreen
    await expect(page.getByTestId('start-screen')).toBeVisible({ timeout: 10000 });
    const startButton = page.getByTestId('start-button');
    await expect(startButton).toBeVisible();
    await startButton.click();

    // 2. SystemCheckScreen
    await expect(page.getByTestId('system-check-screen')).toBeVisible();
    const continueButton = page.getByTestId('check-continue-button');
    await expect(continueButton).toBeEnabled({ timeout: 10000 });
    await continueButton.click();

    // 3. Onboarding Wizard: Step 1 (Video)
    await expect(page.getByTestId('onboarding-panel')).toBeVisible();
    await expect(page.getByTestId('step-counter')).toHaveText('Шаг 1 из 6');
    await expect(page.getByTestId('video-step')).toBeVisible();
    await page.getByTestId('onboarding-next-button').click();

    // Step 2 (Audio)
    await expect(page.getByTestId('step-counter')).toHaveText('Шаг 2 из 6');
    await expect(page.getByTestId('audio-step')).toBeVisible();
    await page.getByTestId('onboarding-next-button').click();

    // Step 3 (Replay)
    await expect(page.getByTestId('step-counter')).toHaveText('Шаг 3 из 6');
    await expect(page.getByTestId('replay-step')).toBeVisible();
    await page.getByTestId('choose-folder-button').click();
    await expect(page.locator('#output-directory')).toHaveValue('C:/Clips');
    await page.getByTestId('onboarding-next-button').click();

    // Step 4 (Hotkeys)
    await expect(page.getByTestId('step-counter')).toHaveText('Шаг 4 из 6');
    await expect(page.getByTestId('hotkeys-step')).toBeVisible();
    await page.getByTestId('onboarding-next-button').click();

    // Step 5 (Preferences)
    await expect(page.getByTestId('step-counter')).toHaveText('Шаг 5 из 6');
    await expect(page.getByTestId('preferences-step')).toBeVisible();
    await page.getByTestId('onboarding-next-button').click();

    // Step 6 (Test)
    await expect(page.getByTestId('step-counter')).toHaveText('Шаг 6 из 6');
    await expect(page.getByTestId('test-step')).toBeVisible();

    const completeBtn = page.getByTestId('complete-onboarding-button');
    await expect(completeBtn).toHaveCount(0);

    // Run test
    const runTestBtn = page.getByTestId('run-test-button');
    await runTestBtn.click();

    // Wait for test to complete and Replay button to be enabled
    await expect(page.getByTestId('test-success-summary')).toBeVisible({ timeout: 15000 });
    await expect(completeBtn).toBeEnabled();

    // Finish onboarding
    await completeBtn.click();

    // 4. Lands on Home Screen
    await expect(page.getByTestId('home-panel')).toBeVisible();
    const persisted = await page.evaluate(() => (window as unknown as {
      firstRunState: { onboarding: { completed: boolean; last_completed_step: number }; active: { onboarding_completed: boolean }; last_successful_test: { succeeded: boolean } };
    }).firstRunState);
    expect(persisted.onboarding).toEqual({ completed: true, last_completed_step: 6 });
    expect(persisted.active.onboarding_completed).toBe(true);
    expect(persisted.last_successful_test.succeeded).toBe(true);
  });
});
