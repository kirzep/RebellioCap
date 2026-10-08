import { t, useTranslation } from '../i18n';
import React from 'react';
import type { AppAction, AppError } from '../bridge/contracts';
import { Button, ErrorNotice } from './Primitives';
import styles from './ErrorPanel.module.css';

export interface ErrorPanelProps {
  error: AppError | string;
  onAction?: (action: AppAction) => void | Promise<void>;
}

const actionLabels: Record<AppAction, string> = {
  retry: 'Повторить',
  diagnostics: 'Открыть диагностику',
  settings: 'Изменить параметры',
  'choose-folder': 'Выбрать папку',
};

function availableActions(error: AppError, hasHandler: boolean): AppAction[] {
  if (!hasHandler) return [];

  return [...new Set(error.actions)].filter(
    (action) => action !== 'retry' || error.retryable
  );
}

function primaryAction(actions: readonly AppAction[]): AppAction | undefined {
  return (
    actions.find((action) => action === 'choose-folder') ??
    actions.find((action) => action === 'settings') ??
    actions.find((action) => action === 'retry') ??
    actions[0]
  );
}

export function ErrorPanel({ error, onAction }: ErrorPanelProps) {
  useTranslation();
  const isStructured = typeof error === 'object' && error !== null;
  const actions: AppAction[] = isStructured
    ? availableActions(error, Boolean(onAction))
    : onAction
      ? ['retry']
      : [];
  const mainAction = primaryAction(actions);

  const technicalDetails = isStructured
    ? [
        t("Код: {0}", error.code),
        t("Подсистема: {0}", error.subsystem),
        t("Сообщение: {0}", error.technicalCause || error.summary),
      ]
        .join('\n')
    : undefined;

  const actionControls =
    actions.length > 0 ? (
      <div className={styles.actions}>
        {actions.map((action) => (
          <Button
            key={action}
            variant={action === mainAction ? 'primary' : 'secondary'}
            onClick={() => onAction?.(action)}
          >
            {actionLabels[action]}
          </Button>
        ))}
      </div>
    ) : undefined;
  const noticeTitle = isStructured ? error.summary : t('Не удалось продолжить');

  return (
    <section className={styles.panel} data-testid="error-panel">
      <ErrorNotice
        title={<h1 className={styles.title}>{t(noticeTitle)}</h1>}
        action={actionControls}
        technicalDetails={technicalDetails}
      >
        {!isStructured ? error : undefined}
      </ErrorNotice>
    </section>
  );
}
