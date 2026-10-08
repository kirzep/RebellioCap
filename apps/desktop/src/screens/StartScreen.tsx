import { t, useTranslation } from '../i18n';
import React from 'react';
import { ArrowRight } from 'lucide-react';
import { AppIcon } from '../components/AppIcon';
import { Button } from '../components/Primitives';
import { SetupShell } from '../onboarding/SetupShell';
import styles from './SetupScreens.module.css';

export interface StartScreenProps { onStart: () => void; }
export function StartScreen({ onStart }: StartScreenProps) {
  useTranslation();
  return (
    <SetupShell phase="welcome">
      <section className={styles.welcome} data-testid="start-screen">
        <p className={styles.eyebrow}>{t("Всё начинается с момента")}</p>
        <h1 className={styles.welcomeTitle}>{t("Хорошие моменты.")}<br /><span>{t("Всегда под рукой.")}</span></h1>
        <p className={styles.welcomeCopy}>{t("Подготовим экран, звук и папку для клипов.")}<br />{t("Дальше — сохраняйте то, что вам важно.")}</p>
        <div className={styles.welcomeFeatures}>
          <div><AppIcon name="replay" size={18} /><p><strong>{t("Мгновенный повтор")}</strong><span>{t("Сохраните то, что только что произошло.")}</span></p></div>
          <div><AppIcon name="video" size={18} /><p><strong>{t("Обычная запись")}</strong><span>{t("Начинается и заканчивается по вашей команде.")}</span></p></div>
        </div>
        <div className={styles.heroActions}>
          <Button variant="primary" trailingIcon={<ArrowRight size={17} />} onClick={onStart} data-testid="start-button">{t("Начать настройку")}</Button>
          <span className={styles.quietNote}>{t("Всего несколько шагов")}</span>
        </div>
      </section>
    </SetupShell>
  );
}
