import { Button } from './ui/button';
import React, { useRef } from 'react';
import styles from './ProgressDots.module.css';

export interface SetupStepDefinition {
  number: number;
  label: string;
  compactLabel: string;
}

export const SETUP_STEPS: readonly SetupStepDefinition[] = [
  { number: 1, label: 'Экран', compactLabel: 'Экран' },
  { number: 2, label: 'Звук', compactLabel: 'Звук' },
  { number: 3, label: 'Повторы и файлы', compactLabel: 'Повторы' },
  { number: 4, label: 'Управление', compactLabel: 'Управление' },
  { number: 5, label: 'Всё верно?', compactLabel: 'Проверка' },
  { number: 6, label: 'Тест записи', compactLabel: 'Тест' },
] as const;

export interface ProgressDotsProps {
  currentStep: number;
  lastCompletedStep: number;
  onSelectStep?: (step: number) => void;
  steps?: readonly SetupStepDefinition[];
  className?: string;
}

interface StepItemProps {
  step: SetupStepDefinition;
  currentStep: number;
  lastCompletedStep: number;
  hasNavigation: boolean;
  compact?: boolean;
  onSelect: (step: number) => void;
}

function StepItem({
  step,
  currentStep,
  lastCompletedStep,
  hasNavigation,
  compact = false,
  onSelect,
}: StepItemProps) {
  const isCurrent = step.number === currentStep;
  const isComplete = step.number <= lastCompletedStep && !isCurrent;
  const isNavigable = step.number <= lastCompletedStep + 1;
  const isDisabled = !hasNavigation || !isNavigable || isCurrent;
  const accessibleState = isCurrent
    ? 'Текущий шаг'
    : !isNavigable
      ? 'Недоступный шаг'
      : !hasNavigation
        ? 'Шаг'
        : 'Перейти к шагу';

  const itemClassName = [
    styles.step,
    isCurrent ? styles.stepCurrent : '',
    isComplete ? styles.stepComplete : '',
    !isNavigable ? styles.stepFuture : '',
    compact ? styles.stepCompact : '',
  ]
    .filter(Boolean)
    .join(' ');

  return (
    <li className={itemClassName} data-testid={`progress-step-${step.number}`}>
      <Button variant="ghost"
        type="button"
        className={styles.stepButton}
        disabled={isDisabled}
        onClick={() => onSelect(step.number)}
        aria-current={isCurrent ? 'step' : undefined}
        aria-label={`${accessibleState} ${step.number}: ${step.label}`}
      >
        <span className={styles.marker} aria-hidden="true">
          {isComplete ? '✓' : step.number}
        </span>
        <span className={styles.label}>{step.label}</span>
      </Button>
    </li>
  );
}

export function ProgressDots({
  currentStep,
  lastCompletedStep,
  onSelectStep,
  steps = SETUP_STEPS,
  className,
}: ProgressDotsProps) {
  const compactNavigationRef = useRef<HTMLDetailsElement>(null);
  const compactSummaryRef = useRef<HTMLElement>(null);
  const activeStep = steps.find((step) => step.number === currentStep) ?? steps[0];
  const navigationClassName = [styles.navigation, className].filter(Boolean).join(' ');

  function selectStep(step: number) {
    if (compactNavigationRef.current) {
      compactNavigationRef.current.open = false;
      compactSummaryRef.current?.focus();
    }
    onSelectStep?.(step);
  }

  return (
    <nav aria-label="Шаги настройки" className={navigationClassName}>
      <ol className={styles.desktopList}>
        {steps.map((step) => (
          <StepItem
            key={step.number}
            step={step}
            currentStep={currentStep}
            lastCompletedStep={lastCompletedStep}
            hasNavigation={Boolean(onSelectStep)}
            onSelect={selectStep}
          />
        ))}
      </ol>

      <details ref={compactNavigationRef} className={styles.compactNavigation}>
        <summary
          ref={compactSummaryRef}
          className={styles.compactSummary}
          aria-current="step"
        >
          <span className={styles.summaryLabel}>
            Шаг {currentStep} из {steps.length}
            {activeStep ? ` · ${activeStep.compactLabel}` : ''}
          </span>
          <span className={styles.summaryHint}>Все шаги</span>
        </summary>
        <ol className={styles.compactList}>
          {steps.map((step) => (
            <StepItem
              key={step.number}
              step={step}
              currentStep={currentStep}
              lastCompletedStep={lastCompletedStep}
              hasNavigation={Boolean(onSelectStep)}
              compact
              onSelect={selectStep}
            />
          ))}
        </ol>
      </details>
    </nav>
  );
}
