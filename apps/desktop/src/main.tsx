import React, { lazy, Suspense } from 'react';
import { installBrowserUiPolicy } from './app/browserUiPolicy';
import { installFrontendLogging } from './app/frontendLogging';
import ReactDOM from 'react-dom/client';
import { NotificationOverlay } from './components/NotificationOverlay';
import './styles/global.css';

// The separate notification WebView never needs the main application modules.
const App = lazy(() => import('./app/App').then(module => ({ default: module.App })));

const overlay = new URLSearchParams(window.location.search).has('overlay');
if (overlay) document.documentElement.classList.add('notification-surface');

installFrontendLogging();
installBrowserUiPolicy();

const rootElement = document.getElementById('root');
if (rootElement) {
  ReactDOM.createRoot(rootElement).render(
    <React.StrictMode>
      {overlay ? <NotificationOverlay /> : <Suspense fallback={<p role="status">Запускаем RebellioCap…</p>}><App /></Suspense>}
    </React.StrictMode>
  );
}
