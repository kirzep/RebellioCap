import React, { useEffect, useRef } from 'react';
import { ArrowLeft, Check } from 'lucide-react';
import logo from '../assets/rebelliocap-logo.svg';
import { DeveloperBrand } from '../components/DeveloperMode';
import { WindowTitlebar } from '../components/WindowTitlebar';
import { SETUP_STEPS } from '../components/ProgressDots';
import { Button } from '../components/Primitives';
import styles from './SetupShell.module.css';

const guidance: Record<number, [string, string]> = {
  1: ['Начните с главного', 'Выберите экран. Мы уже подобрали сбалансированное качество записи.'],
  2: ['Только нужный звук', 'Звук компьютера и микрофон можно включать независимо друг от друга.'],
  3: ['Момент уже в памяти', 'Повтор хранится в памяти. Файл появится только после вашей команды.'],
  4: ['Одно нажатие', 'Сохраните момент, не переключаясь из игры или другого приложения.'],
  5: ['Последний взгляд', 'Любую настройку можно изменить сейчас или позже в приложении.'],
  6: ['Почти готово', 'Короткая запись поможет убедиться, что экран и звук настроены правильно.'],
};

interface SetupShellProps {
  children: React.ReactNode;
  footer?: React.ReactNode;
  step?: number;
  lastCompletedStep?: number;
  onSelectStep?: (step: number) => void;
  onBack?: () => void;
  phase?: 'welcome' | 'check';
}

export function SetupShell({ children, footer, step, lastCompletedStep = 0,
  onSelectStep, onBack, phase }: SetupShellProps) {
  const scrollRef = useRef<HTMLElement>(null);
  useEffect(() => { if (scrollRef.current) scrollRef.current.scrollTop = 0; }, [step, phase]);
  const tip = step ? guidance[step] : undefined;
  return (
    <div className={styles.shell} data-testid="window-shell">
      <WindowTitlebar />
      <div className={styles.layout}>
        <aside className={styles.rail}>
          <div className={styles.brand}><DeveloperBrand /><span>RebellioCap</span></div>
          {step ? <nav className={styles.navigation} aria-label="Шаги настройки">
            <p className={styles.railLabel}>Первый запуск</p>
            <ol>{SETUP_STEPS.map((item) => {
              const current = item.number === step;
              const complete = item.number <= lastCompletedStep && !current;
              return <li key={item.number} data-testid={`progress-step-${item.number}`}>
                <button type="button" className={styles.stepButton} data-current={current}
                  data-complete={complete} aria-current={current ? 'step' : undefined}
                  aria-label={`${current ? 'Текущий шаг' : 'Перейти к шагу'} ${item.number}: ${item.label}`}
                  disabled={current || !onSelectStep || item.number > lastCompletedStep + 1}
                  onClick={() => onSelectStep?.(item.number)}>
                  <span className={styles.marker} aria-hidden="true">{complete ? <Check size={13} /> : String(item.number).padStart(2, '0')}</span>
                  <span className={styles.stepLabel}>{item.label}</span>
                </button>
              </li>;
            })}</ol>
          </nav> : <div className={styles.introduction}>
            <span className={styles.railLabel}>Запись экрана</span>
            <p>Сохраните то,<br />что хочется<br />оставить.</p>
            <div className={styles.replayArt} aria-hidden="true">
              <div className={styles.artFrame}><img src={logo} alt="" /></div>
              <div className={styles.artTimeline}><i /><i /><i /><i /><i /><i /><i /><i /><i /></div>
              <span className={styles.artCaption}>Момент → клип</span>
            </div>
          </div>}
          {tip && <div className={styles.tip} key={step}><strong>{tip[0]}</strong><p>{tip[1]}</p></div>}
          {!step && <p className={styles.railFoot}>Ваш компьютер.<br />Ваши записи.</p>}
        </aside>
        <div className={styles.panel} data-setup-panel>
          <header className={styles.header}>
            <div>{onBack ? <Button variant="tertiary" size="compact" leadingIcon={<ArrowLeft size={16} />}
              onClick={onBack} data-testid="shell-back-button">Назад</Button> : <span>{step ? 'Настройка записи' : phase === 'check' ? 'Проверка компьютера' : 'Добро пожаловать'}</span>}</div>
            {step && <span className={styles.stepCount} data-testid="step-counter">Шаг {step} <span>из 6</span></span>}
          </header>
          {step && <div className={styles.progress} aria-hidden="true"><span style={{ transform: `scaleX(${step / 6})` }} /></div>}
          <main ref={scrollRef} className={styles.content} aria-label={step ? `Настройка записи, шаг ${step} из 6` : 'Первый запуск'}>
            <div className={styles.inner} data-phase={phase}>{children}</div>
          </main>
          {footer && <footer className={styles.footer}>{footer}</footer>}
        </div>
      </div>
    </div>
  );
}
