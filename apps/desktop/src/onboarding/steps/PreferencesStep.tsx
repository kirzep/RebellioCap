import { t, useTranslation } from '../../i18n';
import { Button } from '../../components/Primitives';
import React, { useEffect, useMemo, useState } from 'react';
import type { HostBridge } from '../../bridge/contracts';
import {
  formatContainer,
  formatHotkey,
  formatSeconds,
  formatVideoMode,
  friendlyMonitorName,
} from '../../config/format';
import type { AudioCatalog, AudioSelection, MonitorChoice, OnboardingDraft } from '../../config/model';
import styles from './steps.module.css';

export type SummaryEditStep = 1 | 2 | 3 | 4;

export interface SummaryBlockingIssue {
  step: SummaryEditStep;
  message: string;
}

export interface PreferencesStepProps {
  draft: OnboardingDraft;
  hostBridge?: HostBridge;
  onChange: (patch: Partial<OnboardingDraft>) => void;
  onEditStep?: (step: SummaryEditStep) => void;
  onBlockingIssueChange?: (issue: SummaryBlockingIssue | null) => void;
  onBusyChange?: (busy: boolean) => void;
}

function audioName(
  selection: AudioSelection | null | undefined,
  choices: AudioCatalog['system_audio'],
  catalogLoaded: boolean
): string {
  if (selection === 'disabled') return t('Не записывать');
  if (!selection) return t('Не выбрано');

  const choice = choices.find((item) => item.id === selection.endpoint);
  if (choice) return choice.available ? choice.name : t("{0} · недоступно", choice.name);
  return catalogLoaded
    ? t('Устройство недоступно')
    : t('Получаем название устройства…');
}

export function PreferencesStep({
  draft,
  hostBridge,
  onChange,
  onEditStep,
  onBlockingIssueChange,
  onBusyChange,
}: PreferencesStepProps) {
  useTranslation();
  const [monitors, setMonitors] = useState<MonitorChoice[]>([]);
  const [audioCatalog, setAudioCatalog] = useState<AudioCatalog>({
    system_audio: [],
    microphones: [],
  });
  const [monitorCatalogLoaded, setMonitorCatalogLoaded] = useState(false);
  const [audioCatalogLoaded, setAudioCatalogLoaded] = useState(false);
  const [catalogWarning, setCatalogWarning] = useState<string | null>(null);
  const [catalogBusy, setCatalogBusy] = useState(Boolean(hostBridge));

  useEffect(() => {
    if (draft.preferences == null) {
      onChange({
        preferences: {
          start_with_windows: false,
        },
      });
    }
    // Existing values are deliberately preserved; these flags have no controls in the new UI.
  }, []);

  useEffect(() => {
    const bridge = hostBridge;
    if (!bridge) {
      setCatalogBusy(false);
      onBusyChange?.(false);
      return;
    }
    let cancelled = false;
    setCatalogBusy(true);
    onBusyChange?.(true);

    async function loadCatalogs() {
      const [monitorResult, audioResult] = await Promise.allSettled([
        bridge!.listMonitors(),
        bridge!.listAudioEndpoints(),
      ]);
      if (cancelled) return;

      const warnings: string[] = [];
      if (monitorResult.status === 'fulfilled') {
        setMonitors(monitorResult.value);
        setMonitorCatalogLoaded(true);
      } else {
        warnings.push(t('экран'));
      }

      if (audioResult.status === 'fulfilled') {
        setAudioCatalog(audioResult.value);
        setAudioCatalogLoaded(true);
      } else {
        warnings.push(t('аудиоустройства'));
      }

      if (warnings.length > 0) {
        setCatalogWarning(`Не удалось повторно проверить: ${warnings.join(' и ')}. Ниже показаны сохранённые значения.`);
      }
      setCatalogBusy(false);
      onBusyChange?.(false);
    }

    void loadCatalogs();
    return () => {
      cancelled = true;
      onBusyChange?.(false);
    };
  }, [hostBridge, onBusyChange]);

  const selectedMonitor = monitors.find((monitor) => monitor.id === draft.monitor_id);
  const systemEndpoint =
    draft.system_audio && typeof draft.system_audio === 'object'
      ? draft.system_audio.endpoint
      : null;
  const microphoneEndpoint =
    draft.microphone && typeof draft.microphone === 'object'
      ? draft.microphone.endpoint
      : null;
  const monitorLabel = selectedMonitor
    ? `${friendlyMonitorName(selectedMonitor.name)} · ${selectedMonitor.width}×${selectedMonitor.height}${selectedMonitor.primary ? t(' · Основной') : ''}`
    : draft.monitor_id
      ? monitorCatalogLoaded
        ? t('Экран недоступен')
        : t('Получаем название экрана…')
      : t('Экран не выбран');

  const blockingIssue = useMemo<SummaryBlockingIssue | null>(() => {
    if (draft.monitor_id && monitorCatalogLoaded && !selectedMonitor) {
      return {
        step: 1,
        message: 'Сохранённый экран сейчас недоступен. Выберите другой экран.',
      };
    }
    if (
      systemEndpoint &&
      audioCatalogLoaded &&
      !audioCatalog.system_audio.some(
        (choice) => choice.id === systemEndpoint && choice.available
      )
    ) {
      return {
        step: 2,
        message: 'Устройство системного звука сейчас недоступно. Выберите другое или отключите звук.',
      };
    }
    if (
      microphoneEndpoint &&
      audioCatalogLoaded &&
      !audioCatalog.microphones.some(
        (choice) => choice.id === microphoneEndpoint && choice.available
      )
    ) {
      return {
        step: 2,
        message: 'Сохранённый микрофон сейчас недоступен. Выберите другой или отключите микрофон.',
      };
    }
    return null;
  }, [
    audioCatalog.microphones,
    audioCatalog.system_audio,
    audioCatalogLoaded,
    draft.monitor_id,
    microphoneEndpoint,
    monitorCatalogLoaded,
    selectedMonitor,
    systemEndpoint,
  ]);

  useEffect(() => {
    onBlockingIssueChange?.(blockingIssue);
    return () => onBlockingIssueChange?.(null);
  }, [blockingIssue, onBlockingIssueChange]);

  const rows: Array<{
    step: SummaryEditStep;
    title: string;
    values: Array<{ label: string; value: string; unavailable?: boolean }>;
  }> = [
    {
      step: 1,
      title: t('Экран и видео'),
      values: [
        { label: t('Экран'), value: monitorLabel, unavailable: Boolean(draft.monitor_id && monitorCatalogLoaded && !selectedMonitor) },
        {
          label: t('Видео'),
          value: formatVideoMode(
            draft.width,
            draft.height,
            draft.fps,
            draft.bitrate
          ),
        },
      ],
    },
    {
      step: 2,
      title: t('Звук'),
      values: [
        {
          label: t('Звук компьютера'),
          value: audioName(draft.system_audio, audioCatalog.system_audio, audioCatalogLoaded),
          unavailable: Boolean(
            systemEndpoint &&
            audioCatalogLoaded &&
            !audioCatalog.system_audio.some((choice) => choice.id === systemEndpoint && choice.available)
          ),
        },
        {
          label: t('Микрофон'),
          value: audioName(draft.microphone, audioCatalog.microphones, audioCatalogLoaded),
          unavailable: Boolean(
            microphoneEndpoint &&
            audioCatalogLoaded &&
            !audioCatalog.microphones.some((choice) => choice.id === microphoneEndpoint && choice.available)
          ),
        },
      ],
    },
    {
      step: 3,
      title: t('Replay и файлы'),
      values: [
        {
          label: 'Replay',
          value: `${formatSeconds(draft.replay_seconds)} · ${
            draft.replay_mode === 'disk' ? t('неподдерживаемый дисковый буфер') : t('в памяти')
          }`,
          unavailable: draft.replay_mode === 'disk',
        },
        {
          label: t('Папка и формат'),
          value: `${draft.output_directory || t('Папка не выбрана')} · ${formatContainer(draft.container)}`,
        },
      ],
    },
    {
      step: 4,
      title: t('Управление'),
      values: [
        { label: t('Сохранить Replay'), value: formatHotkey(draft.save_replay_hotkey) },
        {
          label: t('Обычная запись'),
          value: draft.continuous_recording_enabled === false
            ? t("Отключена · {0} сохранено", formatHotkey(draft.toggle_recording_hotkey))
            : formatHotkey(draft.toggle_recording_hotkey),
        },
      ],
    },
  ];

  return (
    <div className={styles.stepContent} data-testid="preferences-step">
      <div className={styles.stepIntro}>
        <h2 className={styles.title}>{t("Всё верно?")}</h2>
        <p className={styles.copy}>{t("Проверьте настройки перед первым клипом.")}</p>
      </div>

      {catalogWarning && <p className={styles.inlineWarning} role="status">{catalogWarning}</p>}
      {catalogBusy && <p className={styles.fieldHint} role="status">{t("Проверяем доступность устройств…")}</p>}

      <div className={styles.summaryTable}>
        {rows.map((row) => (
          <section key={row.step} className={styles.summaryGroup}>
            <div className={styles.summaryGroupHeader}>
              <h3>{row.title}</h3>
              <Button
                type="button"
                variant="tertiary" size="compact"
                aria-label={t("Изменить: {0}", row.title)}
                onClick={() => onEditStep?.(row.step)}
                disabled={!onEditStep}
                data-testid={`summary-edit-step-${row.step}`}
              >
                {t("Изменить")}</Button>
            </div>
            <dl>
              {row.values.map((item) => (
                <div key={item.label} className={styles.summaryItem}>
                  <dt>{t(item.label)}</dt>
                  <dd className={item.unavailable ? styles.unavailableValue : undefined}>{item.value}</dd>
                </div>
              ))}
            </dl>
          </section>
        ))}
      </div>

      <p className={styles.fieldHint}>{t("Крестик сворачивает приложение в трей. Запись продолжает работать.")}</p>
    </div>
  );
}
