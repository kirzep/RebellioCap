import { expect, it } from 'vitest';
import config from '../../src-tauri/tauri.conf.json';

it('lets HTML block drags reach the constructor in the Windows webview', () => {
  expect(config.app.windows.find(window => window.label === 'main')?.dragDropEnabled).toBe(false);
});
