import { t, useTranslation } from '../../i18n';
import { Field, FieldGroup, FieldSet, FieldLegend, FieldLabel } from '../../components/ui/field';
import { Input } from '../../components/ui/input';
import { RadioGroup, RadioGroupItem } from '../../components/ui/radio-group';
import { NativeSelect, NativeSelectOption } from '../../components/ui/native-select';
import React, { useEffect, useState } from 'react';
import type { AppError, HostBridge } from '../../bridge/contracts';
import { normalizeError } from '../../bridge/host';
import { Button, ErrorNotice } from '../../components/Primitives';
import { estimateReplayMegabytes } from '../../config/format';
import type { Container, OnboardingDraft } from '../../config/model';
import type { ValidationError } from '../../config/validation';
import styles from './steps.module.css';

export interface ReplayStepProps {
  draft: OnboardingDraft;
  hostBridge: HostBridge;
  onChange: (patch: Partial<OnboardingDraft>) => void;
  errors?: ValidationError[];
  memoryBudgetBytes?: number;
}

const DURATION_PRESETS = [15, 30, 60, 120] as const;

export function ReplayStep({ draft, hostBridge, onChange, errors = [], memoryBudgetBytes }: ReplayStepProps) {
  useTranslation();
  const [memoryInput, setMemoryInput] = useState(String(draft.replay_memory_limit_mb ?? ''));
  const [memoryEditing, setMemoryEditing] = useState(false);
  useEffect(() => { if (!memoryEditing) setMemoryInput(String(draft.replay_memory_limit_mb ?? '')); }, [draft.replay_memory_limit_mb, memoryEditing]);
  const [folderError, setFolderError] = useState<AppError | null>(null);
  const [customDuration, setCustomDuration] = useState(() =>
    draft.replay_seconds != null &&
    !DURATION_PRESETS.includes(
      draft.replay_seconds as (typeof DURATION_PRESETS)[number]
    )
  );
  const [durationInput, setDurationInput] = useState(() =>
    draft.replay_seconds == null ? '' : String(draft.replay_seconds)
  );
  const [durationEditing, setDurationEditing] = useState(false);

  useEffect(() => {
    const patch: Partial<OnboardingDraft> = {};
    if (draft.replay_seconds == null) patch.replay_seconds = 30;
    if (draft.replay_mode == null) patch.replay_mode = 'ram';
    if (draft.container == null) patch.container = 'mp4';
    if (Object.keys(patch).length > 0) onChange(patch);
    // Defaults only initialize missing values on mount.
  }, []);

  useEffect(() => {
    if (!durationEditing) {
      setDurationInput(
        draft.replay_seconds == null ? '' : String(draft.replay_seconds)
      );
    }
  }, [draft.replay_seconds, durationEditing]);

  const replaySeconds = draft.replay_seconds;
  const durationIsPreset =
    replaySeconds != null &&
    DURATION_PRESETS.includes(
      replaySeconds as (typeof DURATION_PRESETS)[number]
    );
  const durationPreset =
    !customDuration && durationIsPreset
      ? String(replaySeconds)
      : 'custom';
  const memoryValidation = errors.find(error => error.field === 'replay_memory_limit_mb');
  const manualMemory = draft.replay_memory_limit_mb !== undefined && draft.replay_memory_limit_mb !== 0;
  const container: Container | null | undefined = draft.container;
  const estimateMb = estimateReplayMegabytes(draft.bitrate, replaySeconds);
  const durationValidation = errors.find((error) => error.field === 'replay_seconds');
  const replayModeValidation = errors.find((error) => error.field === 'replay_mode');
  const directoryValidation = errors.find((error) => error.field === 'output_directory');
  const containerValidation = errors.find((error) => error.field === 'container');

  async function handlePickFolder() {
    setFolderError(null);
    try {
      const folder = await hostBridge.chooseOutputDirectory();
      if (folder !== null) onChange({ output_directory: folder });
    } catch (error) {
      setFolderError(normalizeError(error));
    }
  }

  return (
    <div className={styles.stepContent} data-testid="replay-step">
      <div className={styles.stepIntro}>
        <h2 className={styles.title}>{t("Повторы и файлы")}</h2>
        <p className={styles.copy}>
          {t("Выберите, сколько последних секунд сохранять и куда складывать клипы.")}</p>
      </div>

      <FieldGroup className={styles.formStack}>
        <Field className={styles.fieldPlain}>
          <FieldLabel htmlFor="replay-duration-preset" className={styles.fieldLabel}>{t("Длительность Replay")}</FieldLabel>
          <NativeSelect
            id="replay-duration-preset"
            className="w-full"
            value={durationPreset}
            aria-invalid={Boolean(durationValidation)}
            aria-describedby={
              durationValidation
                ? 'replay-duration-hint replay-duration-error'
                : 'replay-duration-hint'
            }
            onChange={(event) => {
              if (event.target.value === 'custom') {
                setCustomDuration(true);
                setDurationInput(
                  replaySeconds == null ? '' : String(replaySeconds)
                );
              } else {
                const nextSeconds = Number(event.target.value);
                setCustomDuration(false);
                setDurationInput(String(nextSeconds));
                onChange({ replay_seconds: nextSeconds });
              }
            }}
          >
            {DURATION_PRESETS.map((seconds) => (
              <NativeSelectOption key={seconds} value={seconds}>{seconds} {' '}{t("секунд")}</NativeSelectOption>
            ))}
            <NativeSelectOption value="custom">{t("Другая длительность")}</NativeSelectOption>
          </NativeSelect>

          {durationPreset === 'custom' && (
            <FieldLabel className={styles.inputLabel} htmlFor="replay-seconds">
              {t("Секунды")}<Input
                id="replay-seconds"
                type="text"
                inputMode="numeric"
                className="w-full"
                value={durationInput}
                aria-invalid={Boolean(durationValidation)}
                aria-describedby={
                  durationValidation
                    ? 'replay-duration-hint replay-duration-error'
                    : 'replay-duration-hint'
                }
                onFocus={() => setDurationEditing(true)}
                onBlur={() => setDurationEditing(false)}
                onChange={(event) => {
                  const nextValue = event.target.value;
                  const normalized = nextValue.trim();
                  const isInteger = /^\d+$/.test(normalized);
                  setDurationInput(nextValue);
                  onChange({
                    replay_seconds:
                      normalized === '' || !isInteger ? null : Number(normalized),
                  });
                }}
                data-testid="replay-seconds-input"
              />
            </FieldLabel>
          )}
          <p id="replay-duration-hint" className={styles.fieldHint}>
            {durationPreset === 'custom' ? t('От 1 секунды до 60 минут. ') : ''}
            {estimateMb != null ? t("Примерно {0} МБ видеобуфера в памяти, плюс звук.", estimateMb) : t('Буфер хранится в оперативной памяти.')}
          </p>
          {durationValidation && (
            <p id="replay-duration-error" className={styles.fieldError} role="alert">
              {t(durationValidation.message)}
            </p>
          )}
        </Field>

        <Field className={styles.fieldPlain}>
          <FieldLabel htmlFor="replay-memory-mode" className={styles.fieldLabel}>{t("Лимит памяти Replay")}</FieldLabel>
          <NativeSelect id="replay-memory-mode" value={manualMemory ? 'manual' : 'auto'}
            onChange={event => onChange({ replay_memory_limit_mb: event.target.value === 'auto' ? 0 : 1024 })}>
            <NativeSelectOption value="auto">{t("Авто")}</NativeSelectOption>
            <NativeSelectOption value="manual">{t("Задать вручную")}</NativeSelectOption>
          </NativeSelect>
          {manualMemory && <FieldLabel htmlFor="replay-memory-limit" className={styles.inputLabel}>
            {t("Лимит, МиБ")}<Input id="replay-memory-limit" type="text" inputMode="numeric"
              value={memoryInput} onFocus={() => setMemoryEditing(true)} onBlur={() => setMemoryEditing(false)}
              aria-invalid={Boolean(memoryValidation)} aria-describedby={memoryValidation ? "replay-memory-hint replay-memory-error" : "replay-memory-hint"}
              onChange={event => { setMemoryInput(event.target.value); const value = event.target.value.trim(); onChange({ replay_memory_limit_mb: /^\d+$/.test(value) && Number(value) > 0 ? Number(value) : null }); }} />
          </FieldLabel>}
          <p id="replay-memory-hint" className={styles.fieldHint}>
            {manualMemory ? t('От 64 до 8192 МиБ. При запуске лимит должен помещаться в половину свободной RAM и четверть общей RAM.') : t('Авто рассчитывает лимит по доступной RAM, максимум 1024 МиБ.')}
            {' '}{t("Это верхняя граница памяти для буфера и сохраняемых данных; память заранее не резервируется.")}{memoryBudgetBytes != null && memoryBudgetBytes > 0 && t(' Новое значение вступит в силу после применения настроек.')}
          </p>
          {memoryValidation && <p id="replay-memory-error" className={styles.fieldError} role="alert">{t(memoryValidation.message)}</p>}
        </Field>

        <p className={styles.inlineWarning} role="status">
          {t("Replay автоматически сокращается при достижении лимита памяти.\r\n          Сохранённый клип может быть короче выбранной длительности.")}{memoryBudgetBytes != null && memoryBudgetBytes > 0
            ? t(" Общий лимит буфера и сохраняемых данных в текущей сессии: {0} МиБ.", Math.floor(memoryBudgetBytes / 1024 / 1024))
            : t(' Общий лимит буфера и сохраняемых данных определяется выбранной настройкой при запуске.')}
        </p>

        {draft.replay_mode !== 'ram' && (
          <div
            className={styles.inlineError}
            role="alert"
            data-testid="replay-disk-unsupported"
          >
            <span id="replay-mode-error">
              {replayModeValidation?.message ?? t('Сохранённый дисковый буфер больше не поддерживается.')}{' '}
              {t("Replay работает только в оперативной памяти.")}</span>
            <Button
              type="button"
              variant="secondary"
              aria-describedby="replay-mode-error"
              onClick={() => onChange({ replay_mode: 'ram' })}
              data-testid="replay-mode-ram-button"
            >
              {t("Использовать RAM")}</Button>
          </div>
        )}
        <Field className={styles.fieldPlain}>
          <FieldLabel htmlFor="output-directory" className={styles.fieldLabel}>{t("Папка клипов")}</FieldLabel>
          <div className={styles.pathRow}>
            <Input
              id="output-directory"
              type="text"
              className="w-full"
              value={draft.output_directory ?? ''}
              placeholder={t("Папка не выбрана")}
              readOnly
              aria-invalid={Boolean(directoryValidation)}
              aria-describedby={
                directoryValidation || folderError
                  ? [
                      directoryValidation ? 'output-directory-error' : '',
                      folderError ? 'output-directory-picker-error' : '',
                    ].filter(Boolean).join(' ')
                  : undefined
              }
              data-testid="storage-readout"
            />
            <Button
              type="button"
              variant="secondary"
              aria-describedby={directoryValidation ? 'output-directory-error' : undefined}
              onClick={() => void handlePickFolder()}
              data-testid="choose-folder-button"
            >
              {t("Выбрать папку")}</Button>
          </div>
          {directoryValidation && (
            <p id="output-directory-error" className={styles.fieldError} role="alert">
              {t(directoryValidation.message)}
            </p>
          )}
          {folderError && (
            <ErrorNotice
              id="output-directory-picker-error"
              title={t("Не удалось выбрать папку")}
              technicalDetails={t("Код: {0}\n{1}", folderError.code, folderError.technicalCause)}
              action={(
                <Button size="compact" variant="tertiary" onClick={() => void handlePickFolder()}>
                  {t("Повторить")}</Button>
              )}
              data-testid="folder-error"
            >
              {t(folderError.summary)}
            </ErrorNotice>
          )}
        </Field>

        <FieldSet
          className={styles.fieldset}
          aria-invalid={Boolean(containerValidation)}
          aria-describedby={
            containerValidation
              ? 'recording-container-hint recording-container-error'
              : 'recording-container-hint'
          }
        >
          <FieldLegend className={styles.fieldLabel}>{t("Формат файла")}</FieldLegend>
          <RadioGroup value={container ?? ''} onValueChange={(value) => {
            if (value === 'mp4' || value === 'mkv') onChange({ container: value });
          }} className={styles.radioRow} aria-label={t("Формат файла")}>
            {(['mp4', 'mkv'] as const).map((value) => (
              <label key={value} className={styles.radioLabel}>
                <RadioGroupItem
                  value={value}
                  aria-invalid={Boolean(containerValidation)}
                  data-testid={`container-${value}-button`}
                />
                <span>{value.toUpperCase()}</span>
              </label>
            ))}
          </RadioGroup>
          <p id="recording-container-hint" className={styles.fieldHint}>
            {t("MP4 подходит для большинства плееров и редакторов.")}</p>
          {containerValidation && (
            <p id="recording-container-error" className={styles.fieldError} role="alert">
              {t(containerValidation.message)}
            </p>
          )}
        </FieldSet>
      </FieldGroup>
    </div>
  );
}
