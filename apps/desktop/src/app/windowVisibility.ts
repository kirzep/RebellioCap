// WebView2 can leave document.visibilityState visible when its HWND is minimized.
// Native state is refreshed on window events, never by a polling timer.
const listeners = new Set<() => void>();
let nativeHidden = false;
let stop: (() => void) | undefined;
export function isWindowHidden() { return document.visibilityState === 'hidden' || nativeHidden; }
export function subscribeWindowVisibility(callback: () => void) {
  listeners.add(callback);
  if (!stop) stop = observe();
  return () => {
    listeners.delete(callback);
    if (!listeners.size) { stop?.(); stop = undefined; nativeHidden = false; }
  };
}
function observe() {
  let disposed = false;
  let generation = 0;
  const unlisten: (() => void)[] = [];
  const notify = () => { for (const listener of listeners) listener(); };
  document.addEventListener('visibilitychange', notify);
  if ('__TAURI_INTERNALS__' in window) {
    void import('@tauri-apps/api/window').then(api => {
      const window = api.getCurrentWindow();
      // Browser fixtures may implement only the window controls they exercise.
      if (!window.isMinimized || !window.isVisible || !window.onFocusChanged || !window.onResized) return;
      const refresh = async () => {
        const current = ++generation;
        try {
          const [minimized, visible] = await Promise.all([window.isMinimized(), window.isVisible()]);
          if (disposed || current !== generation) return;
          const hidden = minimized || !visible;
          if (nativeHidden !== hidden) { nativeHidden = hidden; notify(); }
        } catch { /* DOM visibility remains available during a transient IPC failure. */ }
      };
      const registered = (remove: () => void) => { if (disposed) remove(); else unlisten.push(remove); };
      // Even a late registration is unregistered after StrictMode/unmount cleanup.
      void window.onFocusChanged(() => { void refresh(); }).then(registered).catch(() => {});
      void window.onResized(() => { void refresh(); }).then(registered).catch(() => {});
      if (!disposed) void refresh();
    }).catch(() => {});
  }
  return () => {
    disposed = true; generation++;
    document.removeEventListener('visibilitychange', notify);
    for (const remove of unlisten) remove();
  };
}
