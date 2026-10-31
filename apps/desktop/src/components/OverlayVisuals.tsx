import { t } from '../i18n';
import { Circle, Repeat2 } from 'lucide-react';
import replayStartedIcon from '../assets/notifications/replay-started.svg';
import replaySavedIcon from '../assets/notifications/replay-saved.svg';
import recordingStartedIcon from '../assets/notifications/recording-started.svg';
import recordingStoppedIcon from '../assets/notifications/recording-stopped.svg';
import recordingErrorIcon from '../assets/notifications/recording-error.svg';
import replayStoppedIcon from '../assets/notifications/replay-stopped.svg';
import engineStoppedIcon from '../assets/notifications/engine-stopped.svg';
import replayErrorIcon from '../assets/notifications/replay-error.svg';
import './NotificationOverlay.css';
export const actions: Record<string, { text: string; icon: string; tone: string }> = {
  replay_saving: { text: 'Сохраняем повтор', icon: '', tone: 'pending' },
  replay_started: { text: 'Мгновенный повтор включён', icon: replayStartedIcon, tone: 'neutral' },
  replay_saved: { text: 'Повтор сохранён', icon: replaySavedIcon, tone: 'success' },
  recording_started: { text: 'Запись началась', icon: recordingStartedIcon, tone: 'recording' },
  recording_stopped: { text: 'Запись завершена', icon: recordingStoppedIcon, tone: 'neutral' },
  recording_error: { text: 'Ошибка записи', icon: recordingErrorIcon, tone: 'error' },
  replay_stopped: { text: 'Мгновенный повтор выключен', icon: replayStoppedIcon, tone: 'neutral' },
  engine_stopped: { text: 'Движок остановлен из-за ошибки', icon: engineStoppedIcon, tone: 'error' },
  replay_error: { text: 'Не удалось сохранить повтор', icon: replayErrorIcon, tone: 'error' },
};

export function OverlayIndicator({ recording = false }: { recording?: boolean }) {
 return <span className={`capture-indicator${recording ? ' capture-indicator--recording' : ''}`}>
  {recording ? <Circle size={16} fill="currentColor"/> : <Repeat2 size={18}/>}
 </span>;
}
export function OverlayNotification({ kind, closing = false, preview = false }: { kind: string; closing?: boolean; preview?: boolean }) {
 const action = actions[kind];
 if (!action) return null;
 return <div role={preview ? undefined : 'status'} className={`capture-notification ${closing ? 'is-closing' : ''} ${preview ? 'capture-notification--preview' : ''}`} data-tone={action.tone}>
  <div className="capture-notification__icon">{kind === 'replay_saving' ? <span className="capture-save-spinner"/> : <img src={action.icon} alt="" draggable={false} width={42} height={42}/>}</div>
  <strong className="capture-notification__text">{t(action.text)}</strong>
 </div>;
}
