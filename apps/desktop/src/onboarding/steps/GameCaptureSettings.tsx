import { useEffect, useState } from 'react';
import type { HostBridge } from '../../bridge/contracts';
import type { GameWindowChoice, OnboardingDraft } from '../../config/model';
import type { ValidationError } from '../../config/validation';
import { t } from '../../i18n';
import { Field, FieldLabel } from '../../components/ui/field';
import { NativeSelect, NativeSelectOption } from '../../components/ui/native-select';
import { Button } from '../../components/Primitives';
import styles from './steps.module.css';

export function GameCaptureSettings({ draft, hostBridge, onChange, errors }: {
  draft: OnboardingDraft;
  hostBridge: HostBridge;
  onChange: (patch: Partial<OnboardingDraft>) => void;
  errors: ValidationError[];
}) {
  const [windows, setWindows] = useState<GameWindowChoice[]>([]);
  const [loading, setLoading] = useState(true);
  const [failed, setFailed] = useState(false);
  const [refresh, setRefresh] = useState(0);
  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    setFailed(false);
    hostBridge.listGameWindows().then((choices) => {
      if (!cancelled) setWindows(choices);
    }).catch(() => {
      if (!cancelled) { setWindows([]); setFailed(true); }
    }).finally(() => { if (!cancelled) setLoading(false); });
    return () => { cancelled = true; };
  }, [hostBridge, refresh]);

  // The stable selection is an executable, never the foreground HWND or PID.
  const choices = windows.filter((window, index) =>
    windows.findIndex((other) => other.executable.toLowerCase() === window.executable.toLowerCase()) === index);
  const selected = choices.some((window) => window.executable.toLowerCase() === draft.game_executable?.toLowerCase());
  const error = errors.find((item) => item.field === 'game_executable');
  return <Field className={styles.fieldPlain}>
    <FieldLabel htmlFor="video-game" className={styles.fieldLabel}>{t('Игра')}</FieldLabel>
    <NativeSelect id="video-game" value={draft.game_executable ?? ''} className="w-full"
      aria-invalid={Boolean(error)} aria-describedby={error ? 'video-game-error' : 'video-game-help'}
      onChange={(event) => onChange({ game_executable: event.target.value })}>
      <NativeSelectOption value="">{t('Выберите игру из списка')}</NativeSelectOption>
      {draft.game_executable && !selected && <NativeSelectOption value={draft.game_executable}>
        {t('Ожидание игры')} · {draft.game_executable}
      </NativeSelectOption>}
      {choices.map((window) => <NativeSelectOption key={window.executable}
        value={window.executable.toLowerCase() === draft.game_executable?.toLowerCase() ? draft.game_executable : window.executable}>
        {window.title} · {window.executable}
      </NativeSelectOption>)}
    </NativeSelect>
    {error && <p id="video-game-error" className={styles.fieldError} role="alert">{t(error.message)}</p>}
    <div className={styles.compactActions}>
      <Button variant="tertiary" size="compact" busy={loading} onClick={() => setRefresh((value) => value + 1)}>
        {t('Обновить список игр')}
      </Button>
    </div>
    {failed && <p className={styles.inlineWarning} role="status">{t('Не удалось получить список игр. Обновите список.')}</p>}
    {!loading && !failed && choices.length === 0 && <p className={styles.fieldHint}>{t('Запустите игру и обновите список. Сохранённая игра останется выбранной.')}</p>}
    <p id="video-game-help" className={styles.fieldHint}>{t('Другие окна не попадают в видео. Встроенные в игру оверлеи могут остаться. При сворачивании может сохраняться последний кадр игры. Звук настраивается отдельно: системный звук может включать другие приложения.')}</p>
  </Field>;
}
