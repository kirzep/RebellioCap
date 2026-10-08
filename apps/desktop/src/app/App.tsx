import { invoke } from '@tauri-apps/api/core';
import { requestNavigation } from '../editor/navigationGuard';
import React, { useCallback, useEffect, useRef, useState } from 'react';
import { listen } from '@tauri-apps/api/event';
import { ErrorNotice } from '../components/Primitives';
import type { AppAction, AppError, HostBridge } from '../bridge/contracts';
import { host as defaultHost, normalizeError } from '../bridge/host';
import type { Bootstrap, EngineSnapshot, StoredState } from '../config/model';
import { ErrorPanel } from '../components/ErrorPanel';
import type { RuntimeSettingsSection } from '../components/RuntimeSettings';
import { WindowShell } from '../components/WindowShell';
import { LoadingScreen } from '../screens/LoadingScreen';
import { StartScreen } from '../screens/StartScreen';
import { SystemCheckScreen } from '../screens/SystemCheckScreen';
import { HomeScreen } from '../screens/HomeScreen';
import { OnboardingScreen } from '../onboarding/OnboardingScreen';
import { bootstrap, resolveBootstrapResult, type BootstrapResult } from './bootstrap';
import { repairSectionForIssue, type Route } from './route';

export interface AppProps {
  hostBridge?: HostBridge;
}

export function App({ hostBridge = defaultHost }: AppProps) {
  const [route, setRoute] = useState<Route>({ kind: 'loading' });
  const [state, setState] = useState<StoredState | null>(null);
  const [snapshot, setSnapshot] = useState<EngineSnapshot | null>(null);
  const [error, setError] = useState<AppError | null>(null);
  const [trayError, setTrayError] = useState<AppError | null>(null);
  const [openHomeSettings, setOpenHomeSettings] = useState(false);
  const [homeSettingsSection, setHomeSettingsSection] =
    useState<RuntimeSettingsSection>('video');
  const initialBootstrapRef = useRef<{
    host: HostBridge;
    promise: ReturnType<typeof bootstrap>;
  } | null>(null);
  const bootstrapRequestRef = useRef(0);
  useEffect(() => {
    if (!('__TAURI_INTERNALS__' in window)) return;
    let disposed = false;
    const handleQuit = async () => {
      if (disposed) return;
      try {
        const pending = await invoke<boolean>('take_pending_app_quit');
        if (pending && !disposed) requestNavigation(() => {
          void invoke('quit_application').catch(cause => setTrayError(normalizeError(cause)));
        });
      } catch (cause) { if (!disposed) setTrayError(normalizeError(cause)); }
    };
    const listeners = Promise.all([
      listen('tray-settings', () => {
        setHomeSettingsSection('application');
        setOpenHomeSettings(true);
      }),
      listen('tray-quit', () => { void handleQuit(); }),
      listen<string>('tray-error', event => setTrayError(normalizeError(event.payload))),
    ]);
    void listeners.then(unlisten => { if (disposed) unlisten.forEach(remove => remove()); else void handleQuit(); });
    return () => { disposed = true; void listeners.then(unlisten => unlisten.forEach(remove => remove())); };
  }, []);

  const acceptBootstrapResult = useCallback((result: BootstrapResult) => {
    setState(result.state);
    setSnapshot(result.snapshot);
    setError(null);
    setOpenHomeSettings(result.route.kind === 'home' && Boolean(result.route.repair));
    setHomeSettingsSection(
      result.route.kind === 'home' && result.route.repair
        ? result.route.repair.section
        : 'video'
    );
    setRoute(result.route);
  }, []);

  const loadApplication = useCallback(async () => {
    const requestId = bootstrapRequestRef.current + 1;
    bootstrapRequestRef.current = requestId;
    setRoute({ kind: 'loading' });
    setError(null);
    setState(null);
    setSnapshot(null);
    setOpenHomeSettings(false);
    setHomeSettingsSection('video');
    try {
      const promise = bootstrap(hostBridge);
      initialBootstrapRef.current = { host: hostBridge, promise };
      const result = await promise;
      if (requestId !== bootstrapRequestRef.current) return;
      acceptBootstrapResult(result);
    } catch (cause) {
      if (requestId !== bootstrapRequestRef.current) return;
      const normalized = normalizeError(cause);
      setError(normalized);
      setRoute({ kind: 'blocked', error: normalized });
    }
  }, [acceptBootstrapResult, hostBridge]);

  useEffect(() => {
    let ignore = false;
    const requestId = bootstrapRequestRef.current + 1;
    bootstrapRequestRef.current = requestId;
    setRoute({ kind: 'loading' });
    setError(null);
    setState(null);
    setSnapshot(null);
    setOpenHomeSettings(false);
    setHomeSettingsSection('video');
    if (!initialBootstrapRef.current || initialBootstrapRef.current.host !== hostBridge) {
      initialBootstrapRef.current = {
        host: hostBridge,
        promise: bootstrap(hostBridge),
      };
    }
    const initialBootstrap = initialBootstrapRef.current.promise;

    async function initialize() {
      try {
        const result = await initialBootstrap;
        if (ignore || requestId !== bootstrapRequestRef.current) return;
        acceptBootstrapResult(result);
      } catch (cause) {
        if (ignore || requestId !== bootstrapRequestRef.current) return;
        const normalized = normalizeError(cause);
        setError(normalized);
        setRoute({ kind: 'blocked', error: normalized });
      }
    }

    void initialize();
    return () => {
      ignore = true;
    };
  }, [acceptBootstrapResult, hostBridge]);

  async function acceptCompletedOnboarding(result: Bootstrap) {
    const resolved = await resolveBootstrapResult(hostBridge, result);
    acceptBootstrapResult(resolved);
  }

  async function handleBlockedAction(action: AppAction) {
    if (action === 'retry') {
      await loadApplication();
      return;
    }

    if (action === 'settings' || action === 'choose-folder') {
      if (state?.active) {
        const blocked = route.kind === 'blocked' ? route.error ?? error : error;
        setHomeSettingsSection(repairSectionForIssue(blocked, action));
        setOpenHomeSettings(true);
        setRoute({ kind: 'home' });
      }
      return;
    }

    if (action === 'diagnostics') {
      try {
        const result = await hostBridge.runSystemCheck();
        if (!result.diagnostics_path) {
          throw new Error('Диагностический отчёт не был создан.');
        }
        await hostBridge.openDiagnostics();
      } catch (cause) {
        const normalized = normalizeError(cause);
        setError(normalized);
        setRoute({ kind: 'blocked', error: normalized });
      }
    }
  }

  const rawBlockedError =
    route.kind === 'blocked'
      ? route.error ?? error ?? 'Не удалось запустить RebellioCap.'
      : null;
  const blockedError =
    rawBlockedError && typeof rawBlockedError !== 'string' && !state?.active
      ? {
          ...rawBlockedError,
          actions: rawBlockedError.actions.filter(
            (action) => action !== 'settings' && action !== 'choose-folder'
          ),
        }
      : rawBlockedError;

  return (
    <div className="rebelliocap-app" data-testid="app-root" data-route={route.kind}>
      {trayError && <div role="alert" className="fixed bottom-6 right-6 z-50 max-w-md">
        <ErrorNotice title="Не удалось выполнить действие из трея">{trayError.summary}</ErrorNotice>
        <button type="button" onClick={() => setTrayError(null)}>Закрыть сообщение</button>
      </div>}
      {route.kind === 'loading' && <LoadingScreen />}

      {route.kind === 'start' && (
        <StartScreen onStart={() => setRoute({ kind: 'system-check' })} />
      )}

      {route.kind === 'system-check' && (
        <SystemCheckScreen
          hostBridge={hostBridge}
          onContinue={() => setRoute({ kind: 'onboarding', step: 1 })}
          onBack={() => setRoute({ kind: 'start' })}
        />
      )}

      {route.kind === 'onboarding' && (
        <OnboardingScreen
          hostBridge={hostBridge}
          initialDraft={state?.draft}
          initialStep={route.step}
          initialLastCompletedStep={state?.onboarding.last_completed_step ?? 0}
          onComplete={acceptCompletedOnboarding}
          onBackToStart={() => setRoute({ kind: 'start' })}
        />
      )}

      {route.kind === 'home' && (
        <HomeScreen
          hostBridge={hostBridge}
          initialState={state}
          initialSnapshot={snapshot}
          initialView={openHomeSettings ? 'settings' : 'recording'}
          initialSettingsSection={homeSettingsSection}
        />
      )}

      {route.kind === 'blocked' && (
        <WindowShell
          context="Требуется внимание"
          contentWidth="form"
          contentLabel="Ошибка запуска"
        >
          <ErrorPanel
            error={blockedError ?? 'Не удалось запустить RebellioCap.'}
            onAction={handleBlockedAction}
          />
        </WindowShell>
      )}
    </div>
  );
}
