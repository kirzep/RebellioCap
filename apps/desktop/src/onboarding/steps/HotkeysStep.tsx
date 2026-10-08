import { t, useTranslation } from '../../i18n';
import { Button } from '../../components/Primitives';
import { Checkbox } from '../../components/ui/checkbox';
import React, { useEffect, useState } from 'react';
import {
  DEFAULT_SAVE_REPLAY_HOTKEY,
  DEFAULT_TOGGLE_RECORDING_HOTKEY,
} from '../../config/defaults';
import type { Hotkey, OnboardingDraft } from '../../config/model';
import { formatHotkey, formatReplaySaveLabel } from '../../config/format';
import {
  areHotkeysEqual,
  isValidHotkey,
  type ValidationError,
} from '../../config/validation';
import styles from './steps.module.css';

export interface HotkeysStepProps {
  draft: OnboardingDraft;
  onChange: (patch: Partial<OnboardingDraft>) => void;
  onBeforeCapture?: (target: 'replay' | 'recording') => Promise<boolean>;
  onCaptureEnd?: (changed: boolean) => void;
  errors?: ValidationError[];
}

type HotkeyTarget = 'replay' | 'recording';

export const DEFAULT_REPLAY_HOTKEY: Hotkey = DEFAULT_SAVE_REPLAY_HOTKEY;
export const DEFAULT_RECORDING_HOTKEY: Hotkey = DEFAULT_TOGGLE_RECORDING_HOTKEY;

function virtualKeyFromEvent(event: KeyboardEvent): number | null {
  if (/^Key[A-Z]$/.test(event.code)) return event.code.charCodeAt(3);
  if (/^Digit[0-9]$/.test(event.code)) return event.code.charCodeAt(5);
  const functionMatch = /^F([1-9]|1[0-9]|2[0-4])$/.exec(event.code);
  if (functionMatch) return 0x6f + Number(functionMatch[1]);

  switch (event.code) {
    case 'Tab': return 0x09;
    case 'Escape': return 0x1b;
    case 'Space': return 0x20;
    case 'Delete': return 0x2e;
    default:
      return event.keyCode > 0 ? event.keyCode : null;
  }
}

function hotkeyError(hotkey: Hotkey): string | null {
  if (hotkey.key === 0x09 || hotkey.key === 0x1b) {
    return t('Tab и Esc используются для отмены перехвата. Выберите другую основную клавишу.');
  }
  if (hotkey.win) return t('Сочетания с клавишей Win зарезервированы системой.');
  if (hotkey.key === 0x7b) return t('F12 нельзя зарегистрировать как глобальную горячую клавишу.');
  if (hotkey.alt && hotkey.key === 0x09) return t('Alt + Tab зарезервировано для переключения окон.');
  if (hotkey.alt && hotkey.key === 0x20) return t('Alt + Пробел открывает системное меню окна.');
  if (hotkey.alt && hotkey.key === 0x73) return t('Alt + F4 закрывает активное окно.');
  if ((hotkey.ctrl || hotkey.alt) && hotkey.key === 0x1b) return t('Это сочетание с Esc зарезервировано системой.');
  if (hotkey.ctrl && hotkey.alt && hotkey.key === 0x2e) return t('Ctrl + Alt + Delete зарезервировано системой.');
  if (!isValidHotkey(hotkey)) return t('Эта клавиша не поддерживается. Используйте букву, цифру, F-клавишу, Пробел или Delete.');
  return null;
}

export function HotkeysStep({
  draft,
  onChange,
  onBeforeCapture,
  onCaptureEnd,
  errors = [],
}: HotkeysStepProps) {
  useTranslation();
  const [recordingTarget, setRecordingTarget] = useState<HotkeyTarget | null>(null);
  const [capturePreparing, setCapturePreparing] = useState(false);
  const [captureError, setCaptureError] = useState<string | null>(null);
  const [captureNotice, setCaptureNotice] = useState<string | null>(null);

  const saveReplay = draft.save_replay_hotkey ?? DEFAULT_REPLAY_HOTKEY;
  const toggleRecording = draft.toggle_recording_hotkey ?? DEFAULT_RECORDING_HOTKEY;

  useEffect(() => {
    const patch: Partial<OnboardingDraft> = {};
    if (!draft.save_replay_hotkey) patch.save_replay_hotkey = DEFAULT_REPLAY_HOTKEY;
    if (!draft.toggle_recording_hotkey) patch.toggle_recording_hotkey = DEFAULT_RECORDING_HOTKEY;
    if (draft.continuous_recording_enabled == null) patch.continuous_recording_enabled = true;
    if (Object.keys(patch).length > 0) onChange(patch);
    // Defaults only initialize missing values on mount.
  }, []);

  function cancelCapture(message = 'Изменение отменено.') {
    setRecordingTarget(null);
    setCaptureError(null);
    setCaptureNotice(message);
    onCaptureEnd?.(false);
  }

  function applyHotkey(target: HotkeyTarget, nextHotkey: Hotkey) {
    const validationMessage = hotkeyError(nextHotkey);
    if (validationMessage) {
      setCaptureError(validationMessage);
      return;
    }

    const otherHotkey = target === 'replay' ? toggleRecording : saveReplay;
    if (areHotkeysEqual(nextHotkey, otherHotkey)) {
      setCaptureError('Это сочетание уже назначено другому действию.');
      return;
    }

    onChange(target === 'replay'
      ? { save_replay_hotkey: nextHotkey }
      : { toggle_recording_hotkey: nextHotkey });
    setRecordingTarget(null);
    setCaptureError(null);
    setCaptureNotice(`Сохранено: ${formatHotkey(nextHotkey)}.`);
    onCaptureEnd?.(true);
  }

  useEffect(() => {
    if (!recordingTarget) return;
    const captureTarget = recordingTarget;

    function handleKeyDown(event: KeyboardEvent) {
      if (event.repeat) return;
      if (event.code === 'Tab' || event.key === 'Tab' || event.keyCode === 0x09) {
        cancelCapture(t('Изменение отменено. Фокус переведён дальше.'));
        return;
      }
      if (event.code === 'Escape' || event.key === 'Escape' || event.keyCode === 0x1b) {
        event.preventDefault();
        event.stopPropagation();
        cancelCapture();
        return;
      }
      if (['Control', 'Alt', 'Shift', 'Meta'].includes(event.key)) {
        setCaptureError(null);
        setCaptureNotice('Добавьте основную клавишу к модификатору.');
        return;
      }

      event.preventDefault();
      event.stopPropagation();
      const key = virtualKeyFromEvent(event);
      if (key == null) {
        setCaptureError('Не удалось определить физическую клавишу. Попробуйте другую.');
        return;
      }

      applyHotkey(captureTarget, {
        key,
        ctrl: event.ctrlKey,
        alt: event.altKey,
        shift: event.shiftKey,
        win: event.metaKey,
      });
    }

    window.addEventListener('keydown', handleKeyDown, true);
    return () => window.removeEventListener('keydown', handleKeyDown, true);
  }, [recordingTarget, saveReplay, toggleRecording, onChange]);

  async function beginCapture(target: HotkeyTarget) {
    if (recordingTarget === target) {
      cancelCapture();
      return;
    }

    if (recordingTarget) cancelCapture();
    if (onBeforeCapture) {
      setCapturePreparing(true);
      setCaptureError(null);
      setCaptureNotice('Подготавливаем перехват клавиш…');
      try {
        const ready = await onBeforeCapture(target);
        if (!ready) {
          setCapturePreparing(false);
          setCaptureNotice(null);
          setCaptureError(null);
          return;
        }
      } catch (error) {
        setCapturePreparing(false);
        setCaptureNotice(null);
        setCaptureError(error instanceof Error ? error.message : 'Не удалось подготовить перехват клавиш.');
        return;
      }
      setCapturePreparing(false);
    }
    setRecordingTarget(target);
    setCaptureError(null);
    setCaptureNotice('Нажмите сочетание. Esc или Tab отменит изменение.');
  }

  const duplicate = areHotkeysEqual(saveReplay, toggleRecording);
  const replayValidation = errors.find((error) => error.field === 'save_replay_hotkey');
  const recordingValidation = errors.find(
    (error) => error.field === 'toggle_recording_hotkey'
  );
  const duplicateValidation = errors.find((error) => error.field === 'duplicate_hotkeys');
  const continuousValidation = errors.find(
    (error) => error.field === 'continuous_recording_enabled'
  );
  const hasDuplicateError = duplicate || Boolean(duplicateValidation);
  const replayDescribedBy = [
    replayValidation ? 'save-replay-hotkey-error' : '',
    hasDuplicateError ? 'hotkeys-duplicate-error' : '',
    captureError && recordingTarget === 'replay' ? 'hotkey-capture-error' : '',
  ].filter(Boolean).join(' ') || undefined;
  const recordingDescribedBy = [
    recordingValidation ? 'toggle-recording-hotkey-error' : '',
    hasDuplicateError ? 'hotkeys-duplicate-error' : '',
    captureError && recordingTarget === 'recording' ? 'hotkey-capture-error' : '',
  ].filter(Boolean).join(' ') || undefined;

  return (
    <div className={styles.stepContent} data-testid="hotkeys-step">
      <div className={styles.stepIntro}>
        <h2 className={styles.title}>{t("Горячие клавиши")}</h2>
        <p className={styles.copy}>{t("Сохраняйте момент одним нажатием. Эти сочетания работают в любом приложении.")}</p>
      </div>

      <div className={styles.hotkeyList}>
        <div className={styles.hotkeyRow}>
          <div>
            <span className={styles.fieldLabel}>
              {formatReplaySaveLabel(draft.replay_seconds ?? 30)}
            </span>
            <kbd className={styles.hotkeyValue} data-testid="hotkey-replay-display">
              {recordingTarget === 'replay' ? t('Ожидание сочетания…') : formatHotkey(saveReplay)}
            </kbd>
            {replayValidation && (
              <p id="save-replay-hotkey-error" className={styles.fieldError} role="alert">
                {t(replayValidation.message)}
              </p>
            )}
          </div>
          <Button
            type="button"
            variant={recordingTarget === 'replay' ? 'primary' : 'secondary'}
            aria-label={
              recordingTarget === 'replay'
                ? t('Отменить изменение сочетания для сохранения Replay')
                : t('Изменить сочетание для сохранения Replay')
            }
            aria-invalid={Boolean(
              replayValidation || hasDuplicateError || (captureError && recordingTarget === 'replay')
            )}
            aria-describedby={replayDescribedBy}
            onClick={() => void beginCapture('replay')}
            disabled={
              capturePreparing ||
              (recordingTarget !== null && recordingTarget !== 'replay')
            }
            data-testid="record-replay-hotkey-button"
          >
            {recordingTarget === 'replay' ? t('Отмена') : t('Изменить')}
          </Button>
        </div>

        <div className={styles.hotkeyRow}>
          <div>
            <span className={styles.fieldLabel}>{t("Начать / остановить запись")}</span>
            <kbd className={styles.hotkeyValue} data-testid="hotkey-recording-display">
              {recordingTarget === 'recording' ? t('Ожидание сочетания…') : formatHotkey(toggleRecording)}
            </kbd>
            {recordingValidation && (
              <p id="toggle-recording-hotkey-error" className={styles.fieldError} role="alert">
                {t(recordingValidation.message)}
              </p>
            )}
          </div>
          <Button
            type="button"
            variant={recordingTarget === 'recording' ? 'primary' : 'secondary'}
            aria-label={
              recordingTarget === 'recording'
                ? t('Отменить изменение сочетания обычной записи')
                : t('Изменить сочетание обычной записи')
            }
            aria-invalid={Boolean(
              recordingValidation || hasDuplicateError || (captureError && recordingTarget === 'recording')
            )}
            aria-describedby={recordingDescribedBy}
            onClick={() => void beginCapture('recording')}
            disabled={
              capturePreparing ||
              (recordingTarget !== null && recordingTarget !== 'recording')
            }
            data-testid="record-recording-hotkey-button"
          >
            {recordingTarget === 'recording' ? t('Отмена') : t('Изменить')}
          </Button>
        </div>
      </div>

      {recordingTarget && (
        <div className={styles.capturePanel} role="status">
          <p>{t("Нажмите сочетание с буквой, цифрой или F-клавишей.")}</p>
          <p className={styles.fieldHint}>{t("Esc — отменить. Tab — отменить и перейти дальше.")}</p>
        </div>
      )}

      {captureError && (
        <p
          id="hotkey-capture-error"
          className={styles.fieldError}
          role="alert"
          data-testid="hotkey-capture-error"
        >
          {captureError}
        </p>
      )}
      {hasDuplicateError && (
        <p
          id="hotkeys-duplicate-error"
          className={styles.fieldError}
          role="alert"
          data-testid="hotkeys-duplicate-error"
        >
          {duplicateValidation?.message ?? t('Горячие клавиши не должны совпадать.')}
        </p>
      )}
      {captureNotice && !captureError && !recordingTarget && (
        <p className={styles.fieldHint} aria-live="polite">{captureNotice}</p>
      )}

      <label className={styles.checkboxRow}>
        <Checkbox
          checked={draft.continuous_recording_enabled ?? true}
          disabled={Boolean(recordingTarget) || capturePreparing}
          aria-invalid={Boolean(continuousValidation)}
          aria-describedby={
            continuousValidation ? 'continuous-recording-enabled-error' : undefined
          }
          onCheckedChange={(checked) => onChange({ continuous_recording_enabled: checked === true })}
          data-testid="continuous-recording-checkbox"
        />
        <span>
          <strong>{t("Разрешить обычную запись")}</strong>
          <small>
            {draft.continuous_recording_enabled === false
              ? t('Команда обычной записи будет недоступна. Сочетание сохранится на будущее.')
              : t('Сочетание запускает и останавливает отдельную непрерывную запись.')}
          </small>
        </span>
      </label>
      {continuousValidation && (
        <p id="continuous-recording-enabled-error" className={styles.fieldError} role="alert">
          {t(continuousValidation.message)}
        </p>
      )}

      <Button
        type="button"
        variant="tertiary" size="compact"
        disabled={Boolean(recordingTarget) || capturePreparing}
        onClick={() => {
          const changed =
            !areHotkeysEqual(saveReplay, DEFAULT_REPLAY_HOTKEY) ||
            !areHotkeysEqual(toggleRecording, DEFAULT_RECORDING_HOTKEY);
          onChange({
            save_replay_hotkey: DEFAULT_REPLAY_HOTKEY,
            toggle_recording_hotkey: DEFAULT_RECORDING_HOTKEY,
          });
          setCaptureNotice('Стандартные сочетания восстановлены.');
          setCaptureError(null);
          setRecordingTarget(null);
          onCaptureEnd?.(changed);
        }}
        data-testid="hotkeys-reset-button"
      >
        {t("Восстановить стандартные")}</Button>
    </div>
  );
}
