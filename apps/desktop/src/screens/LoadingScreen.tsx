import { t, useTranslation } from '../i18n';
import React from 'react';
import { WindowShell } from '../components/WindowShell';
import styles from './SetupScreens.module.css';

export function LoadingScreen() {
  useTranslation();
  return (
    <WindowShell context={t("Запуск")} contentWidth="form" contentLabel={t("Запуск RebellioCap")}>
      <div className={`${styles.page} ${styles.loading}`} data-testid="loading-screen">
        <h1 className="sr-only">{t("Запуск RebellioCap")}</h1>
        <span className={styles.spinner} aria-hidden="true" />
        <p className={styles.loadingText} role="status" aria-live="polite">
          {t("Запускаем RebellioCap…")}</p>
      </div>
    </WindowShell>
  );
}
