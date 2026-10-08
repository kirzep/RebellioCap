import { t, useTranslation } from '../i18n';
import {NativeSelect,NativeSelectOption} from './ui/native-select';
import { useEffect, useRef, useState, type CSSProperties, type KeyboardEvent } from 'react';
import { Dialog } from 'radix-ui';
import { AudioLines, LoaderCircle, Maximize, Mic, Minimize, Monitor, Pause, Play, RotateCcw, RotateCw, SlidersHorizontal, Volume2, VolumeX, X } from 'lucide-react';
import type { ClipPlayback } from '../bridge/contracts';
import styles from './ClipPlayer.module.css';
import { isWindowHidden, subscribeWindowVisibility } from '../app/windowVisibility';

function clock(seconds: number) {
  const total = Math.max(0, Math.floor(Number.isFinite(seconds) ? seconds : 0));
  const hours = Math.floor(total / 3600);
  return `${hours ? `${hours}:` : ''}${String(Math.floor(total / 60) % 60).padStart(2, '0')}:${String(total % 60).padStart(2, '0')}`;
}
function rangeFill(value: number): CSSProperties { return { '--fill': `${value}%` } as CSSProperties; }

export function ClipPlayer({ name, media, onClose, onRelease }: {
  name: string; media: ClipPlayback; onClose: () => void; onRelease?: (id: string) => void;
}) {
  useTranslation();
  const video = useRef<HTMLVideoElement>(null);
  const audio = useRef<(HTMLAudioElement | null)[]>([]);
  const panel = useRef<HTMLDivElement>(null);
  const soundPanel = useRef<HTMLElement>(null);
  const soundButton = useRef<HTMLButtonElement>(null);
  const hideControls = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  const [mixerOpen, setMixerOpen] = useState(false);
  const [controlsVisible, setControlsVisible] = useState(true);
  const [volumes, setVolumes] = useState<number[]>(() => media.tracks.map(track => track.enabled ? 100 : 0));
  const previousVolumes = useRef(media.tracks.map(() => 100));
  const [playing, setPlaying] = useState(false);
  const [buffering, setBuffering] = useState(true);
  const [position, setPosition] = useState(0);
  const [duration, setDuration] = useState(0);
  const [resolution, setResolution] = useState('');
  const [speed, setSpeed] = useState(1);
  const [fullscreen, setFullscreen] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const release = useRef(onRelease);
  const disposal = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  const autoplay = useRef(!isWindowHidden());
  release.current = onRelease;
  const soundId = `${media.id}-audio`;

  function revealControls() {
    setControlsVisible(true);
    clearTimeout(hideControls.current);
    if (playing && !mixerOpen) hideControls.current = setTimeout(() => setControlsVisible(false), 2400);
  }
  function closeMixer() {
    setMixerOpen(false);
    soundButton.current?.focus();
  }

  function readMetadata() {
    const master = video.current;
    if (!master) return;
    setDuration(Number.isFinite(master.duration) ? master.duration : 0);
    if (master.videoWidth) setResolution(`${master.videoWidth} × ${master.videoHeight}`);
    setPosition(master.currentTime);
  }
  function seek(seconds: number) {
    const master = video.current;
    if (!master || !Number.isFinite(master.duration) || master.duration <= 0) return;
    master.currentTime = Math.max(0, Math.min(master.duration, seconds));
    setPosition(master.currentTime); synchronize(true);
  }
  function togglePlayback() {
    const master = video.current;
    if (!master) return;
    if (!master.paused) master.pause();
    else {
      setError(null);
      void master.play().catch(error => {
        if (error instanceof DOMException && error.name === 'AbortError') return;
        setBuffering(false); setError('Не удалось запустить клип. Попробуйте ещё раз.');
      });
    }
  }
  function setVolume(index: number, value: number) {
    if (value > 0) previousVolumes.current[index] = value;
    setVolumes(previous => previous.map((volume, i) => i === index ? value : volume));
  }
  function changeSpeed(value: number) {
    if (video.current) video.current.playbackRate = value;
    setSpeed(value); synchronize();
  }
  async function toggleFullscreen() {
    try {
      if (document.fullscreenElement) await document.exitFullscreen();
      else await panel.current?.requestFullscreen();
    } catch { setError('Не удалось включить полноэкранный режим.'); }
  }
  function keyboard(event: KeyboardEvent<HTMLDivElement>) {
    if (event.ctrlKey || event.altKey || event.metaKey || event.target instanceof HTMLElement && event.target.closest('input, select, button, [contenteditable="true"]')) return;
    if (event.key === ' ' || event.code === 'KeyK') { event.preventDefault(); togglePlayback(); }
    if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') { event.preventDefault(); seek((video.current?.currentTime ?? 0) + (event.key === 'ArrowRight' ? 5 : -5)); }
    if (event.code === 'KeyF') { event.preventDefault(); void toggleFullscreen(); }
  }

  function synchronize(force = false) {
    const master = video.current;
    if (!master) return;
    for (const track of audio.current) {
      if (!track) continue;
      track.playbackRate = master.playbackRate;
      if (force || Math.abs(track.currentTime - master.currentTime) > 0.08) track.currentTime = master.currentTime;
    }
  }
  function pauseAudio() { audio.current.forEach(track => track?.pause()); }
  function pauseWhenHidden() {
    if (!isWindowHidden()) return false;
    video.current?.pause();
    pauseAudio();
    setPlaying(false);
    return true;
  }
  function playAudio() {
    if (pauseWhenHidden() || !video.current || video.current.paused || video.current.seeking) return;
    synchronize(true);
    for (const track of audio.current) {
      if (!track) continue;
      void track.play().catch(error => {
        if (error instanceof DOMException && error.name === 'AbortError') return;
        video.current?.pause();
        setError('Не удалось воспроизвести аудиодорожку. Попробуйте снова запустить клип.');
      });
    }
  }
  useEffect(() => {
    audio.current.forEach((track, index) => { if (track) track.volume = volumes[index] / 100; });
  }, [volumes]);
  useEffect(() => {
    if (disposal.current !== undefined) clearTimeout(disposal.current);
    return () => {
      const elements = [video.current, ...audio.current];
      // StrictMode re-runs effects on mount; only dispose after an actual unmount.
      disposal.current = setTimeout(() => {
        for (const element of elements) {
          if (!element) continue;
          element.pause(); element.removeAttribute('src'); element.load();
        }
        release.current?.(media.id);
      }, 0);
    };
  }, [media.id]);
  useEffect(() => {
    if (!playing) return;
    const timer = window.setInterval(() => { if (video.current && !video.current.paused && !video.current.seeking) synchronize(); }, 100);
    return () => window.clearInterval(timer);
  }, [playing]);
  useEffect(() => {
    pauseWhenHidden();
    return subscribeWindowVisibility(pauseWhenHidden);
  }, []);
  useEffect(() => {
    const update = () => setFullscreen(document.fullscreenElement === panel.current);
    document.addEventListener('fullscreenchange', update);
    return () => document.removeEventListener('fullscreenchange', update);
  }, []);
  useEffect(() => {
    revealControls();
    return () => clearTimeout(hideControls.current);
  }, [playing, mixerOpen]);
  useEffect(() => {
    if (!mixerOpen) return;
    const dismiss = (event: PointerEvent) => {
      if (event.target instanceof Node && !soundPanel.current?.contains(event.target) && !soundButton.current?.contains(event.target)) setMixerOpen(false);
    };
    document.addEventListener('pointerdown', dismiss);
    return () => document.removeEventListener('pointerdown', dismiss);
  }, [mixerOpen]);

  const activeTracks = volumes.filter(volume => volume > 0).length;
  const channels = media.tracks.map((track, index) => ({ track, index })).sort((a, b) => Number(a.track.label === t('Системный звук + микрофон')) - Number(b.track.label === t('Системный звук + микрофон')));

  return <Dialog.Root open onOpenChange={open => { if (!open) onClose(); }}>
    <Dialog.Portal><Dialog.Overlay className={styles.backdrop} /><Dialog.Content ref={panel} className={styles.player} data-controls-visible={controlsVisible} aria-describedby={undefined} onKeyDown={keyboard}
      onPointerMove={revealControls} onPointerDown={revealControls} onFocusCapture={revealControls}
      onEscapeKeyDown={event => { if (mixerOpen) { event.preventDefault(); closeMixer(); } }}
      onOpenAutoFocus={event => { event.preventDefault(); panel.current?.focus(); }}>
      <header className={styles.header}>
        <div className={styles.heading}><Dialog.Title title={name}>{name}</Dialog.Title></div>
        {resolution && <span className={styles.metadata}>{resolution}</span>}
        <Dialog.Close className={styles.close} aria-label={t("Закрыть просмотр")} title={t("Закрыть · Esc")}><X size={18} /></Dialog.Close>
      </header>
      <div className={styles.stage} data-playing={playing}>
        <video ref={element => { if (element) video.current = element; }} src={media.video} autoPlay={autoplay.current} muted playsInline preload="auto" aria-label={t("Видео клипа")}
          onClick={togglePlayback} onDoubleClick={() => void toggleFullscreen()} onLoadedMetadata={readMetadata} onDurationChange={readMetadata}
          onLoadedData={() => setBuffering(false)} onCanPlay={() => setBuffering(false)} onTimeUpdate={() => setPosition(video.current?.currentTime ?? 0)}
          onPlay={() => { if (pauseWhenHidden()) return; setPlaying(true); setError(null); playAudio(); }} onPlaying={() => { if (pauseWhenHidden()) return; setBuffering(false); playAudio(); }}
          onPause={() => { setPlaying(false); pauseAudio(); }} onEnded={() => { setPlaying(false); pauseAudio(); }}
          onWaiting={() => { setBuffering(true); pauseAudio(); }} onSeeking={() => { pauseAudio(); synchronize(true); }}
          onSeeked={() => { setPosition(video.current?.currentTime ?? 0); if ((video.current?.readyState ?? 0) >= 2) setBuffering(false); synchronize(true); playAudio(); }}
          onRateChange={() => { setSpeed(video.current?.playbackRate ?? 1); synchronize(); }}
          onError={() => { pauseAudio(); setPlaying(false); setBuffering(false); setError('Не удалось воспроизвести видео. Возможно, кодек не поддерживается системой.'); }} />
        {buffering && !error ? <div className={styles.loading} role="status"><LoaderCircle size={28} /><span>{t("Загружаем видео…")}</span></div>
          : !playing && !error && <button className={styles.centerPlay} onClick={togglePlayback} aria-label={t("Воспроизвести клип")}><Play size={27} fill="currentColor" strokeWidth={1.4} /></button>}
        <div className={styles.controls}>
          <input className={`${styles.range} ${styles.timeline}`} type="range" min="0" max={duration || 0} step="0.01" value={Math.min(position, duration)} disabled={!duration}
            aria-label={t("Позиция воспроизведения")} aria-valuetext={t("{0} из {1}", clock(position), clock(duration))} style={rangeFill(duration ? position / duration * 100 : 0)} onChange={event => seek(Number(event.target.value))} />
          <div className={styles.transport}>
            <button className={styles.playButton} onClick={togglePlayback} aria-label={playing ? t('Пауза') : t('Воспроизвести')} title={playing ? t('Пауза · Пробел') : t('Воспроизвести · Пробел')}>{playing ? <Pause size={18} fill="currentColor" /> : <Play size={18} fill="currentColor" />}</button>
            <button className={styles.iconButton} onClick={() => seek((video.current?.currentTime ?? 0) - 5)} disabled={!duration} aria-label={t("Назад на 5 секунд")} title={t("Назад на 5 секунд · ←")}><RotateCcw size={19} /><span className={styles.skipAmount}>5</span></button>
            <button className={styles.iconButton} onClick={() => seek((video.current?.currentTime ?? 0) + 5)} disabled={!duration} aria-label={t("Вперёд на 5 секунд")} title={t("Вперёд на 5 секунд · →")}><RotateCw size={19} /><span className={styles.skipAmount}>5</span></button>
            <span className={styles.time}><span>{clock(position)}</span><span className={styles.timeDivider}>/</span><span>{clock(duration)}</span></span>
            <div className={styles.transportEnd}>
              {media.tracks.length > 0 && <button ref={soundButton} className={styles.iconButton} data-active={mixerOpen} onClick={() => setMixerOpen(open => !open)}
                aria-label={t("Звук и дорожки")} aria-expanded={mixerOpen} aria-controls={soundId} title={t("Звук и дорожки")}>{activeTracks ? <Volume2 size={19} /> : <VolumeX size={19} />}</button>}
              <label className={styles.speed}><NativeSelect aria-label={t("Скорость воспроизведения")} value={speed} onChange={event => changeSpeed(Number(event.target.value))}>{[0.5, 0.75, 1, 1.25, 1.5, 2].map(value => <NativeSelectOption key={value} value={value}>{value}×</NativeSelectOption>)}</NativeSelect></label>
              <button className={styles.iconButton} onClick={() => void toggleFullscreen()} aria-label={fullscreen ? t('Выйти из полноэкранного режима') : t('Полноэкранный режим')} title={t("Полноэкранный режим · F")}>{fullscreen ? <Minimize size={18} /> : <Maximize size={18} />}</button>
            </div>
          </div>
        </div>
      {media.tracks.length > 0 && <section ref={soundPanel} id={soundId} className={styles.mixer} hidden={!mixerOpen} aria-label={t("Аудиомикшер")}
        onKeyDown={event => { if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); closeMixer(); } }}>
        <div className={styles.mixerHeading}><h3><SlidersHorizontal size={14} />{t("Звук и дорожки")}</h3><button className={styles.close} onClick={closeMixer} aria-label={t("Закрыть настройки звука")}><X size={15} /></button></div>
        <div className={styles.tracks}>{channels.map(({ track, index }) => {
          const mixed = track.label === t('Системный звук + микрофон');
          const microphone = track.label === t('Микрофон');
          const enabled = volumes[index] > 0;
          const Icon = mixed ? AudioLines : microphone ? Mic : track.label === t('Системный звук') ? Monitor : Volume2;
          return <div key={track.path} className={`${styles.channel} ${mixed ? styles.mixed : ''}`} data-muted={!enabled}>
            <div className={styles.channelHeading}>
              <span className={styles.channelIcon}><Icon size={19} strokeWidth={1.6} /></span>
              <div className={styles.channelName}><span>{mixed ? t('Объединённая дорожка') : track.label}</span>{mixed && <small>{t("Микрофон + системный звук")}</small>}</div>
              <button className={styles.muteButton} data-muted={!enabled} onClick={() => setVolume(index, enabled ? 0 : previousVolumes.current[index])}
                aria-label={`${enabled ? t('Отключить') : t('Включить')} ${track.label}`} aria-pressed={!enabled} title={enabled ? t('Отключить дорожку') : t('Включить дорожку')}>{enabled ? <Volume2 size={17} /> : <VolumeX size={17} />}</button>
            </div>
            <div className={styles.channelVolume}><input className={styles.range} type="range" min="0" max="100" value={volumes[index]} aria-label={t("Громкость {0}", track.label)} style={rangeFill(volumes[index])}
              onChange={event => setVolume(index, Number(event.target.value))} /><output aria-label={t("Уровень {0}", track.label)}>{volumes[index]}<span>%</span></output></div>
          </div>;
        })}</div>
      </section>}
      </div>
      {media.tracks.map((track, index) => <audio key={track.path} ref={element => { if (element) { audio.current[index] = element; element.volume = volumes[index] / 100; } }} src={track.path} preload="auto"
        onError={() => { video.current?.pause(); setError(`Не удалось воспроизвести дорожку «${track.label}».`); }} />)}
      {error && <p role="alert" className={styles.error}>{t(error)}</p>}
    </Dialog.Content></Dialog.Portal>
  </Dialog.Root>;
}
