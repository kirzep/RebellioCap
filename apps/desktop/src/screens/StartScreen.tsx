import React from 'react';
import { ArrowRight } from 'lucide-react';
import { AppIcon } from '../components/AppIcon';
import { Button } from '../components/Primitives';
import { SetupShell } from '../onboarding/SetupShell';
import styles from './SetupScreens.module.css';

export interface StartScreenProps { onStart: () => void; }
export function StartScreen({ onStart }: StartScreenProps) {
  return (
    <SetupShell phase="welcome">
      <section className={styles.welcome} data-testid="start-screen">
        <p className={styles.eyebrow}>Всё начинается с момента</p>
        <h1 className={styles.welcomeTitle}>Хорошие моменты.<br /><span>Всегда под рукой.</span></h1>
        <p className={styles.welcomeCopy}>Подготовим экран, звук и папку для клипов.<br />Дальше — сохраняйте то, что вам важно.</p>
        <div className={styles.welcomeFeatures}>
          <div><AppIcon name="replay" size={18} /><p><strong>Мгновенный повтор</strong><span>Сохраните то, что только что произошло.</span></p></div>
          <div><AppIcon name="video" size={18} /><p><strong>Обычная запись</strong><span>Начинается и заканчивается по вашей команде.</span></p></div>
        </div>
        <div className={styles.heroActions}>
          <Button variant="primary" trailingIcon={<ArrowRight size={17} />} onClick={onStart} data-testid="start-button">Начать настройку</Button>
          <span className={styles.quietNote}>Всего несколько шагов</span>
        </div>
      </section>
    </SetupShell>
  );
}
