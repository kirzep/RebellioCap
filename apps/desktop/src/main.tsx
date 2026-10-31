import { ErrorCenterProvider } from './components/ErrorCenter';
import React, { lazy, Suspense } from 'react';
import { installBrowserUiPolicy } from './app/browserUiPolicy';
import { installFrontendLogging } from './app/frontendLogging';
import ReactDOM from 'react-dom/client';
import { NotificationOverlay } from './components/NotificationOverlay';
import { OverlayEditorWindow } from './components/OverlayEditorWindow';
import './styles/global.css';
import { initializeLanguage, t } from './i18n';
import { getCurrentWebviewWindow } from '@tauri-apps/api/webviewWindow';

// The separate notification WebView never needs the main application modules.
const App = lazy(() => import('./app/App').then(module => ({ default: module.App })));

const nativeWindowLabel = '__TAURI_INTERNALS__' in window ? getCurrentWebviewWindow().label : undefined;
const overlay = nativeWindowLabel === 'overlay' || new URLSearchParams(window.location.search).has('overlay');
const overlayEditor = nativeWindowLabel === 'overlay-editor';
if (overlay) document.documentElement.classList.add('notification-surface');

installFrontendLogging();
installBrowserUiPolicy();

const rootElement = document.getElementById('root');
async function renderApplication() {
  if (!rootElement) return;
  await initializeLanguage();
  ReactDOM.createRoot(rootElement).render(
    <React.StrictMode>
      {overlay ? <NotificationOverlay /> : overlayEditor ? <ErrorCenterProvider><OverlayEditorWindow /></ErrorCenterProvider> : <Suspense fallback={<p role="status">{t('Запускаем RebellioCap…')}</p>}><App /></Suspense>}
    </React.StrictMode>
  );
}
void renderApplication();
