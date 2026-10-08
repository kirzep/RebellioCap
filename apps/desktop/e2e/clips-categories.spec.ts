import { expect, test } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

if (process.env.PLAYWRIGHT_CHANNEL) test.use({ channel: process.env.PLAYWRIGHT_CHANNEL });

test('clip gallery sorts across categories and navigates folders in the browser', async ({ page }) => {
  const iconBytes = [...readFileSync(resolve(process.env.GAME_ICON_FIXTURE ?? 'src-tauri/icons/icon.ico'))];
  const errors: string[] = [];
  page.on('pageerror', error => { errors.push(error.message); console.error(error.message); });
  await page.route('**/clips-qa', route => route.fulfill({
    contentType: 'text/html',
    body: `<!doctype html><html lang="ru"><head><title>ScopeClipper Clips QA</title></head>
    <body><main id="root" style="padding:32px;max-width:1100px;margin:auto"></main>
    <script type="module">
      import RefreshRuntime from '/@react-refresh';
      RefreshRuntime.injectIntoGlobalHook(window);
      window.$RefreshReg$ = () => {};
      window.$RefreshSig$ = () => type => type;
      window.__vite_plugin_react_preamble_installed__ = true;
    </script><script type="module">
      import React from '/node_modules/.vite/deps/react.js';
      import ReactDOM from '/node_modules/.vite/deps/react-dom_client.js';
      import { ClipsScreen } from '/src/screens/ClipsScreen.tsx';
      import '/src/styles/global.css';
      const clips = [
        { name:'old.mp4', relativePath:'Elden Ring/old.mp4', folder:'Elden Ring', savedMs:10, modifiedMs:99, bytes:1048576 },
        { name:'desktop.mp4', relativePath:'Desktop/desktop.mp4', folder:'Desktop', savedMs:20, modifiedMs:20, bytes:2097152 },
        { name:'latest.mp4', relativePath:'Counter-Strike 2/latest.mp4', folder:'Counter-Strike 2', savedMs:30, modifiedMs:30, bytes:3145728 },
      ];
      const bridge = { listClips:async()=>clips, prepareClipPlayback:async name=>{window.openedClip=name;return {id:'qa',video:'',tracks:[]};},
        folderIcon:async folder=>{if(folder==='Counter-Strike 2') return ${JSON.stringify(iconBytes)}; throw new Error('No icon');} };
      ReactDOM.createRoot(document.getElementById('root')).render(React.createElement(ClipsScreen,{bridge,directory:'D:/Clips'}));
    </script></body></html>`,
  }));
  await page.goto('/clips-qa');
  await expect(page).toHaveTitle('ScopeClipper Clips QA');
  await expect(page.getByRole('button', { name: 'Открыть latest.mp4' })).toBeVisible();
  expect(await page.getByRole('button', { name: /^Открыть / }).allTextContents()).toHaveLength(3);
  expect(await page.getByRole('button', { name: /^Открыть / }).evaluateAll(buttons => buttons.map(b => b.getAttribute('aria-label'))))
    .toEqual(['Открыть latest.mp4', 'Открыть desktop.mp4', 'Открыть old.mp4']);
  await page.getByRole('button', { name: 'По папкам' }).click();
  expect(await page.getByRole('button', { name: /^Открыть папку/ }).evaluateAll(buttons => buttons.map(b => b.getAttribute('aria-label'))))
    .toEqual(['Открыть папку Counter-Strike 2', 'Открыть папку Desktop', 'Открыть папку Elden Ring']);
  const icon = page.getByRole('img', { name: 'Значок Counter-Strike 2' });
  await expect(icon).toBeVisible();
  await expect.poll(() => icon.evaluate(image => (image as HTMLImageElement).naturalWidth)).toBeGreaterThan(0);
  await expect(page.getByLabel('Рабочий стол', { exact: true })).toBeVisible();
  await expect(page.getByLabel('Папка игры', { exact: true })).toBeVisible();
  await page.screenshot({ path: test.info().outputPath('folders-desktop.png') });
  await page.getByRole('button', { name: 'Открыть папку Counter-Strike 2' }).click();
  await expect(page.getByRole('button', { name: 'Открыть latest.mp4' })).toBeVisible();
  await expect(page.getByRole('button', { name: 'Открыть desktop.mp4' })).toHaveCount(0);
  await page.getByRole('button', { name: 'Открыть latest.mp4' }).click();
  expect(await page.evaluate(() => (window as unknown as { openedClip: string }).openedClip)).toBe('Counter-Strike 2/latest.mp4');
  await page.getByRole('button', { name: 'Закрыть просмотр' }).click();
  await page.getByRole('button', { name: /Все папки/ }).click();
  await page.reload();
  await expect(page.getByRole('button', { name: 'По папкам' })).toHaveAttribute('aria-pressed', 'true');
  await page.setViewportSize({ width: 560, height: 800 });
  await page.screenshot({ path: test.info().outputPath('folders-narrow.png') });
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.emulateMedia({forcedColors:'active'});
  await page.getByRole('button',{name:'Открыть папку Counter-Strike 2'}).focus();
  const selectedView = page.getByRole('button', {name:'По папкам',exact:true});
  await expect(selectedView).not.toBeFocused();
  expect(await selectedView.evaluate(element=>parseFloat(getComputedStyle(element).outlineWidth))).toBeGreaterThanOrEqual(2);
  expect(await selectedView.evaluate(element=>getComputedStyle(element).outlineStyle)).not.toBe('none');
  await page.screenshot({path:test.info().outputPath('folders-forced-colors.png')});
  expect(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth)).toBe(true);
  expect(errors).toEqual([]);
});
