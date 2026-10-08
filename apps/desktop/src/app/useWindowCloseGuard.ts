import { useCallback, useEffect, useState } from 'react';
import { getCurrentWindow } from '@tauri-apps/api/window';
import type { AppError } from '../bridge/contracts';
import { normalizeError } from '../bridge/host';

export interface WindowCloseGuardOptions {
  shouldConfirm: boolean;
  onBeforeClose: () => Promise<void>;
}
export interface WindowCloseGuard {
  closeRequested: boolean;
  isClosing: boolean;
  closeError: AppError | null;
  cancelClose: () => void;
  confirmClose: () => Promise<void>;
}

// Closing the window only hides it. Native tray exit owns engine shutdown.
export function useWindowCloseGuard(_options: WindowCloseGuardOptions): WindowCloseGuard {
  const [closeError, setCloseError] = useState<AppError | null>(null);
  const hideWindow = useCallback(async () => {
    if (!('__TAURI_INTERNALS__' in window)) return;
    try {
      await getCurrentWindow().hide();
      setCloseError(null);
    } catch (cause) { setCloseError(normalizeError(cause)); }
  }, []);
  useEffect(() => {
    if (!('__TAURI_INTERNALS__' in window)) return;
    let disposed = false;
    const listener = getCurrentWindow().onCloseRequested(event => {
      event.preventDefault();
      void hideWindow();
    });
    void listener.then(remove => { if (disposed) remove(); })
      .catch(cause => { if (!disposed) setCloseError(normalizeError(cause)); });
    return () => { disposed = true; void listener.then(remove => remove()).catch(() => {}); };
  }, [hideWindow]);
  return {
    closeRequested: false,
    isClosing: false,
    closeError,
    cancelClose: () => setCloseError(null),
    confirmClose: hideWindow,
  };
}