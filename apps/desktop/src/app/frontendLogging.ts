import { invoke } from '@tauri-apps/api/core';

export function installFrontendLogging() {
  if (!('__TAURI_INTERNALS__' in window)) return;
  const send = (level: string, message: string) => { void invoke('log_frontend', { level, message: message.slice(0, 8192) }).catch(() => {}); };
  const describe = (value: unknown) => {
    if (value instanceof Error) return value.stack ?? value.message;
    try { return typeof value === 'string' ? value : JSON.stringify(value); } catch { return String(value); }
  };
  window.addEventListener('error', event => send('error', `${event.message}\n${event.error instanceof Error ? event.error.stack : event.filename}`));
  window.addEventListener('unhandledrejection', event => send('error', `Unhandled rejection: ${describe(event.reason)}`));
  for (const level of ['warn', 'error'] as const) {
    const original = console[level].bind(console);
    console[level] = (...values: unknown[]) => { original(...values); send(level, values.map(describe).join(' ')); };
  }
}
