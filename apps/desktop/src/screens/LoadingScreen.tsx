import React from 'react';
import { WindowShell } from '../components/WindowShell';
import styles from './SetupScreens.module.css';

export function LoadingScreen() {
  return (
    <WindowShell context="Запуск" contentWidth="form" contentLabel="Запуск RebellioCap">
      <div className={`${styles.page} ${styles.loading}`} data-testid="loading-screen">
        <h1 className="sr-only">Запуск RebellioCap</h1>
        <span className={styles.spinner} aria-hidden="true" />
        <p className={styles.loadingText} role="status" aria-live="polite">
          Запускаем RebellioCap…
        </p>
      </div>
    </WindowShell>
  );
}
