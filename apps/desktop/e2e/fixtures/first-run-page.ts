import { expect, type Page } from '@playwright/test';

export async function openFirstRunFixture(page: Page) {
  await page.route('**/first-run-qa', route => route.fulfill({
    contentType: 'text/html',
    body: `<!doctype html><html><body><div id="root"></div><script type="module">import RefreshRuntime from '/@react-refresh';RefreshRuntime.injectIntoGlobalHook(window);window.$RefreshReg$=()=>{};window.$RefreshSig$=()=>type=>type;window.__vite_plugin_react_preamble_installed__=true;</script><script type="module" src="/e2e/fixtures/first-run.tsx"></script></body></html>`,
  }));
  await page.goto('/first-run-qa');
}

export async function completeFirstRunFixture(page: Page) {
  await openFirstRunFixture(page);
  await page.getByTestId('start-button').click();
  await page.getByTestId('check-continue-button').click();
  for (let step = 1; step <= 5; step++) {
    await expect(page.getByTestId('step-counter')).toHaveText(`Шаг ${step} из 6`);
    if (step === 3) await page.getByTestId('choose-folder-button').click();
    await page.getByTestId('onboarding-next-button').click();
  }
  await page.getByTestId('run-test-button').click();
  await page.getByTestId('complete-onboarding-button').click();
  await expect(page.getByTestId('home-panel')).toBeVisible();
}
