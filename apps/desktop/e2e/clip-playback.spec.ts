import { expect, test } from '@playwright/test';
import { execFileSync } from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

if (process.env.PLAYWRIGHT_CHANNEL) test.use({ channel: process.env.PLAYWRIGHT_CHANNEL });

test('plays a real clip with separate synchronized audio and releases it on close', async ({ page }) => {
  const errors: string[] = [];
  page.on('pageerror', error => errors.push(error.message));
  await page.setViewportSize({ width: 1120, height: 780 });
  const source = process.env.CLIP_PLAYBACK_FIXTURE;
  test.skip(!source || !existsSync(source), 'Set CLIP_PLAYBACK_FIXTURE to a recorded clip with three audio tracks');
  const directory = mkdtempSync(join(tmpdir(), 'rebelliocap-player-test-'));
  try {
    const media = JSON.parse(execFileSync(resolve('../../build/windows-hardware-release/RebellioCap.Engine.exe'), ['--prepare-playback', source!, directory], { encoding: 'utf8' }));
    const tracks = media.tracks.map((track: { path: string; label: string }) => ({ ...track, path: `/media/${track.path}`, enabled: track.label !== 'Системный звук + микрофон' }));
    await page.route('**/media/*', route => {
      const name = new URL(route.request().url()).pathname.split('/').pop()!;
      const bytes = readFileSync(join(directory, name));
      const range = route.request().headers().range?.match(/bytes=(\d+)-(\d*)/);
      const start = range ? Number(range[1]) : 0;
      const end = range?.[2] ? Math.min(Number(range[2]), bytes.length - 1) : bytes.length - 1;
      return route.fulfill({ status: range ? 206 : 200, headers: { 'accept-ranges': 'bytes', ...(range ? { 'content-range': `bytes ${start}-${end}/${bytes.length}` } : {}) }, contentType: name.endsWith('.mp4') ? 'video/mp4' : 'audio/mp4', body: bytes.subarray(start, end + 1) });
    });
    await page.route('**/playback-qa', route => route.fulfill({ contentType: 'text/html', body: `<!doctype html><html><head><meta charset="utf-8"></head><body><div id="root"></div>
      <script type="module">import RefreshRuntime from '/@react-refresh'; RefreshRuntime.injectIntoGlobalHook(window);
      window.$RefreshReg$=()=>{};window.$RefreshSig$=()=>type=>type;window.__vite_plugin_react_preamble_installed__=true;</script>
      <script type="module">
      import React from '/node_modules/.vite/deps/react.js';import ReactDOM from '/node_modules/.vite/deps/react-dom_client.js';
      import { ClipsScreen } from '/src/screens/ClipsScreen.tsx';import '/src/styles/global.css';
      const bridge={listClips:async()=>[{name:'real.mp4',bytes:1,modifiedMs:1}],prepareClipPlayback:async()=>(${JSON.stringify({ id: 'real', video: '/media/video.mp4', tracks })}),releaseClipPlayback:async id=>{window.released=id}};
      ReactDOM.createRoot(document.getElementById('root')).render(React.createElement(React.StrictMode,null,React.createElement(ClipsScreen,{bridge})));
      </script></body></html>` }));
    await page.goto('/playback-qa');
    await page.getByRole('button', { name: 'Открыть real.mp4' }).click();
    await expect(page.getByRole('dialog')).toBeVisible();
    await expect.poll(() => page.locator('video').evaluate((video: HTMLVideoElement) => video.readyState)).toBeGreaterThanOrEqual(2);
    expect(await page.locator('audio').evaluateAll(elements => elements.map(element => (element as HTMLAudioElement).volume))).toEqual([0, 1, 1]);
    await expect(page.getByRole('region', { name: 'Аудиомикшер' })).toHaveCount(0);
    const videoSize = await page.locator('video').boundingBox();
    expect(videoSize!.width).toBeGreaterThan(1050);
    expect(videoSize!.height).toBeGreaterThan(700);
    await expect(page.getByRole('dialog')).toHaveAttribute('data-controls-visible', 'false');
    await page.locator('video').hover();
    await expect(page.getByRole('dialog')).toHaveAttribute('data-controls-visible', 'true');
    await page.getByRole('button', { name: 'Звук и дорожки' }).click();
    await page.getByLabel('Громкость Микрофон', { exact: true }).fill('35');
    await page.getByRole('button', { name: 'Отключить Микрофон', exact: true }).click();
    expect(await page.locator('audio').nth(2).evaluate((audio: HTMLAudioElement) => audio.volume)).toBe(0);
    await page.getByRole('button', { name: 'Включить Микрофон', exact: true }).click();
    expect(await page.locator('audio').nth(2).evaluate((audio: HTMLAudioElement) => audio.volume)).toBe(0.35);
    await page.getByRole('button', { name: 'Закрыть настройки звука' }).click();
    await page.locator('video').evaluate((video: HTMLVideoElement) => video.pause());
    await page.getByRole('button', { name: 'Воспроизвести', exact: true }).click();
    await expect.poll(() => page.locator('video').evaluate((video: HTMLVideoElement) => video.currentTime)).toBeGreaterThan(0.3);
    await expect.poll(() => page.locator('audio').evaluateAll(elements => elements.every(element => !(element as HTMLAudioElement).paused))).toBe(true);
    await page.getByRole('combobox', {name:'Скорость воспроизведения'}).click();
    await page.getByRole('option', {name:'1.5×',exact:true}).click();
    expect(await page.locator('audio').evaluateAll(elements => elements.every(element => (element as HTMLAudioElement).playbackRate === 1.5))).toBe(true);
    await page.getByRole('button', { name: 'Пауза', exact: true }).click();
    await page.getByLabel('Позиция воспроизведения').fill('5');
    await expect.poll(() => page.locator('audio').evaluateAll(elements => elements.every(element => Math.abs((element as HTMLAudioElement).currentTime - 5) < 0.1))).toBe(true);
    await expect(page.getByRole('alert')).toHaveCount(0);
    await page.getByRole('dialog').focus();
    await page.keyboard.press('Space');
    await expect(page.getByRole('button', { name: 'Пауза', exact: true })).toBeVisible();
    await page.keyboard.press('Space');
    await expect(page.getByRole('button', { name: 'Воспроизвести', exact: true })).toBeVisible();
    await page.screenshot({ path: test.info().outputPath('player-desktop.png'), animations: 'disabled' });
    await page.getByRole('button', { name: 'Звук и дорожки' }).click();
    await page.screenshot({ path: test.info().outputPath('player-audio.png'), animations: 'disabled' });
    await page.keyboard.press('Escape');
    await expect(page.getByRole('region', { name: 'Аудиомикшер' })).toHaveCount(0);
    await expect(page.getByRole('dialog')).toBeVisible();
    await page.getByRole('button', { name: 'Полноэкранный режим', exact: true }).click();
    await expect(page.getByRole('button', { name: 'Выйти из полноэкранного режима' })).toBeVisible();
    await page.getByRole('button', { name: 'Выйти из полноэкранного режима' }).click();
    await expect.poll(() => page.evaluate(() => document.fullscreenElement === null)).toBe(true);
    await page.setViewportSize({ width: 560, height: 800 });
    await expect(page.getByText('Загружаем видео…')).toHaveCount(0);
    await page.screenshot({ path: test.info().outputPath('player-narrow.png'), animations: 'disabled' });
    expect(await page.getByRole('dialog').evaluate(element => element.scrollWidth <= element.clientWidth)).toBe(true);
    await page.getByRole('button', { name: 'Закрыть просмотр' }).click();
    await expect(page.getByRole('dialog')).toHaveCount(0);
    await expect.poll(() => page.evaluate(() => (window as unknown as { released: string }).released)).toBe('real');
    expect(errors).toEqual([]);
  } finally { rmSync(directory, { recursive: true, force: true }); }
});
