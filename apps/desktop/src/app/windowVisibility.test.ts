import { afterEach, beforeEach, expect, it, vi } from 'vitest';
const native = vi.hoisted(() => ({
  minimized: false, visible: true,
  focus: undefined as undefined | (() => void), resize: undefined as undefined | (() => void),
  isMinimized: vi.fn(), isVisible: vi.fn(), unlisten: vi.fn(),
}));
vi.mock('@tauri-apps/api/window', () => ({ getCurrentWindow: () => ({
  isMinimized: native.isMinimized, isVisible: native.isVisible,
  onFocusChanged: async (callback: () => void) => { native.focus = callback; return native.unlisten; },
  onResized: async (callback: () => void) => { native.resize = callback; return native.unlisten; },
}) }));
beforeEach(() => {
  vi.resetModules(); vi.clearAllMocks();
  native.minimized = false; native.visible = true; native.focus = undefined; native.resize = undefined;
  native.isMinimized.mockImplementation(async () => native.minimized);
  native.isVisible.mockImplementation(async () => native.visible);
  Object.defineProperty(window, '__TAURI_INTERNALS__', { configurable: true, value: {} });
  Object.defineProperty(document, 'visibilityState', { configurable: true, value: 'visible' });
});
afterEach(() => { delete (window as unknown as Record<string, unknown>).__TAURI_INTERNALS__; });
it('uses native minimized/hidden state while WebView still reports visible', async () => {
  const { subscribeWindowVisibility, isWindowHidden } = await import('./windowVisibility');
  const changed = vi.fn(); const unsubscribe = subscribeWindowVisibility(changed);
  await vi.waitFor(() => expect(native.focus).toBeTypeOf('function'));
  expect(isWindowHidden()).toBe(false);
  native.minimized = true; native.focus!();
  await vi.waitFor(() => expect(isWindowHidden()).toBe(true));
  native.minimized = false; native.resize!();
  await vi.waitFor(() => expect(isWindowHidden()).toBe(false));
  native.visible = false; native.focus!();
  await vi.waitFor(() => expect(isWindowHidden()).toBe(true));
  expect(changed).toHaveBeenCalledTimes(3);
  unsubscribe(); expect(native.unlisten).toHaveBeenCalledTimes(2);
});
it('does not treat losing focus to another visible app as hidden', async () => {
  const { subscribeWindowVisibility, isWindowHidden } = await import('./windowVisibility');
  const changed = vi.fn(); const unsubscribe = subscribeWindowVisibility(changed);
  await vi.waitFor(() => expect(native.focus).toBeTypeOf('function'));
  native.focus!(); await Promise.resolve(); await Promise.resolve();
  expect(isWindowHidden()).toBe(false); expect(changed).not.toHaveBeenCalled(); unsubscribe();
});
it('ignores stale native queries and removes listeners resolving after disposal', async () => {
  const { subscribeWindowVisibility, isWindowHidden } = await import('./windowVisibility');
  const unsubscribe = subscribeWindowVisibility(vi.fn());
  await vi.waitFor(() => expect(native.focus).toBeTypeOf('function'));
  let resolveOld!: (value: boolean) => void;
  native.isMinimized.mockImplementationOnce(() => new Promise<boolean>(resolve => { resolveOld = resolve; }));
  native.focus!(); native.minimized = true; native.resize!();
  await vi.waitFor(() => expect(isWindowHidden()).toBe(true));
  resolveOld(false); await Promise.resolve(); await Promise.resolve();
  expect(isWindowHidden()).toBe(true); unsubscribe();
  const delayed = subscribeWindowVisibility(vi.fn()); delayed();
  await vi.waitFor(() => expect(native.unlisten).toHaveBeenCalledTimes(4));
});
