// Real multi-window WebView2 acceptance. Requires a debug desktop build, staged
// engine and the Vite server at 127.0.0.1:1420. Uses an isolated settings profile.
import { chromium, expect } from '@playwright/test';
import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createServer } from 'node:net';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const binary = resolve(process.env.REBELLIOCAP_WEBVIEW_BINARY ?? join(root, 'apps/desktop/src-tauri/target/debug/RebellioCap.exe'));
const engine = resolve(process.env.REBELLIOCAP_WEBVIEW_ENGINE ?? join(root, 'build/windows-debug/RebellioCap.Engine.exe'));
for (const path of [binary, engine]) if (!existsSync(path)) throw new Error(`Missing prerequisite: ${path}`);
const evidence = mkdtempSync(join(tmpdir(), 'rebcap-native-overlay-'));
mkdirSync(join(evidence, 'config'));
const server = createServer();
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const port = server.address().port;
await new Promise(resolve => server.close(resolve));
const child = spawn(binary, [], { cwd: root, windowsHide: true, env: {
  ...process.env, REBELLIOCAP_TEST_CONFIG_ROOT: join(evidence, 'config'),
  REBELLIOCAP_ENGINE_PATH: engine, WEBVIEW2_USER_DATA_FOLDER: join(evidence, 'webview'),
  WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS: `--remote-debugging-port=${port}`,
} });
let browser, main, stdout = '', stderr = '';
child.stdout.on('data', data => { stdout += data; });
child.stderr.on('data', data => { stderr += data; });
const result = { passed: false, checks: [], evidence };
async function withTimeout(promise, milliseconds) {
  let timer;
  try { return await Promise.race([promise, new Promise((_, reject) => { timer = setTimeout(() => reject(new Error('Native operation timed out')), milliseconds); })]); }
  finally { clearTimeout(timer); }
}
const invoke = (page, command, args) => page.evaluate(({ command, args }) => window.__TAURI_INTERNALS__.invoke(command, args), { command, args });
const windowState = (label = 'main') => main.evaluate(async label => {
  const moduleUrl = performance.getEntriesByType('resource').find(entry => /@tauri-apps_api_window\.js/.test(entry.name))?.name;
  if (!moduleUrl) throw new Error('Tauri window API has not loaded');
  const { Window } = await import(moduleUrl);
  const window = new Window(label);
  return { visible: await window.isVisible(), fullscreen: await window.isFullscreen(), position: await window.outerPosition(), size: await window.innerSize() };
}, label);
try {
  const deadline = Date.now() + 30000;
  while (!browser && Date.now() < deadline) {
    if (child.exitCode !== null) throw new Error(`Desktop exited: ${child.exitCode}; ${stderr}`);
    try { browser = await chromium.connectOverCDP(`http://127.0.0.1:${port}`, { timeout: 1000 }); }
    catch { await new Promise(resolve => setTimeout(resolve, 100)); }
  }
  if (!browser) throw new Error('WebView2 connection timed out');
  const context = browser.contexts()[0];
  await expect.poll(() => context.pages().length).toBeGreaterThanOrEqual(2);
  await expect.poll(() => context.pages().some(page => page.url().includes('?overlay'))).toBe(true);
  main = context.pages().find(page => !page.url().includes('?overlay'));
  const overlay = context.pages().find(page => page.url().includes('?overlay'));
  await main.waitForFunction(() => window.__TAURI_INTERNALS__);
  await invoke(main, 'set_language_settings', { preference: 'ru' });
  const preferences = await invoke(main, 'get_notification_settings');
  await invoke(main, 'set_notification_settings', { settings: { ...preferences, overlayEnabled: false, soundEnabled: false } });
  await main.waitForFunction(() => performance.getEntriesByType('resource').some(entry => /@tauri-apps_api_window\.js/.test(entry.name)));
  await expect.poll(async () => (await windowState()).visible).toBe(true);
  const original = await windowState();
  await overlay.evaluate(() => {
    window.__feedback = [];
    new MutationObserver(() => {
      const frame = document.querySelector('.overlay-editor-feedback');
      if (!frame) return;
      const style = getComputedStyle(frame);
      window.__feedback.push({ status: frame.dataset.status, background: style.backgroundColor, body: getComputedStyle(document.body).backgroundColor });
    }).observe(document.body, { subtree: true, childList: true });
  });
  async function open() {
    const opened = context.waitForEvent('page');
    void opened.catch(() => {});
    await withTimeout(invoke(main, 'open_overlay_editor'), 15000);
    const editor = await opened;
    await expect(editor.getByRole('dialog', { name: 'Конструктор оверлея' })).toBeVisible();
    expect(await editor.getByRole('button').count()).toBe(3);
    const current = await windowState();
    expect(current).toEqual({ ...original, visible: false });
    expect((await windowState('overlay-editor')).fullscreen).toBe(true);
    return editor;
  }
  let editor = await open();
  await editor.getByRole('button', { name: 'Статус записи', exact: true }).press('ArrowLeft');
  await editor.getByRole('dialog').press('Control+Enter');
  await expect.poll(() => editor.isClosed()).toBe(true);
  await expect.poll(() => overlay.evaluate(() => window.__feedback.some(frame => frame.status === 'saved'))).toBe(true);
  await expect.poll(async () => (await windowState()).visible).toBe(true);
  const saved = await invoke(main, 'get_notification_settings');
  expect(saved.layout.recording.x).toBeCloseTo(preferences.layout.recording.x - .005);
  expect(await windowState()).toEqual(original);
  result.checks.push('Separate fullscreen editor saves layout without changing main window geometry');
  editor = await open();
  await editor.getByRole('button', { name: 'Статус записи', exact: true }).press('ArrowRight');
  await editor.getByRole('dialog').press('Escape');
  await expect.poll(() => editor.isClosed()).toBe(true);
  await expect.poll(async () => (await windowState()).visible).toBe(true);
  expect((await invoke(main, 'get_notification_settings')).layout).toEqual(saved.layout);
  const feedback = await overlay.evaluate(() => window.__feedback);
  for (const status of ['saved', 'cancelled']) {
    expect(feedback.some(frame => frame.status === status && frame.background === 'rgba(0, 0, 0, 0)' && frame.body === 'rgba(0, 0, 0, 0)')).toBe(true);
  }
  await expect(overlay.getByRole('status')).toHaveCount(0);
  expect((await windowState('overlay')).visible).toBe(false);
  result.checks.push('Cancellation keeps saved layout; desktop feedback stays transparent and closes while notifications are disabled');
  result.passed = true;
} catch (error) {
  result.failure = error.stack ?? String(error);
  process.exitCode = 1;
} finally {
  if (main && !main.isClosed()) await withTimeout(invoke(main, 'quit_application'), 3000).catch(() => {});
  await browser?.close().catch(() => {});
  if (child.exitCode === null) child.kill();
  writeFileSync(join(evidence, 'result.json'), JSON.stringify(result, null, 2));
  writeFileSync(join(evidence, 'stdout.log'), stdout);
  writeFileSync(join(evidence, 'stderr.log'), stderr);
  console.log(JSON.stringify(result, null, 2));
}
