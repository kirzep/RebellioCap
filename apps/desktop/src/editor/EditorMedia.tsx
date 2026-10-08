import { useEffect, useRef, type CSSProperties } from 'react';
import type { Asset, Item } from './model';
import type { PlaybackClock } from './playbackClock';

type Props = {
  item: Item;
  asset?: Asset;
  clock: PlaybackClock;
  active: boolean;
  playing: boolean;
  canvasWidth: number;
  onPlaybackError: (message: string) => void;
};

/** Owns one preview element. The editor owns assets and the shared clock.
 * Seeking follows the current trim interval; play is attempted only when the
 * transport or active/source state changes, never on every clock tick.
 */
export function EditorMedia({ item, asset, clock, active, playing, canvasWidth, onPlaybackError }: Props) {
  const ref = useRef<HTMLMediaElement | null>(null);
  const wantsPlayback = useRef(false);
  const audioIndex = asset?.audio.indexOf(item.stream) ?? -1;
  const src = item.kind === 'audio' ? (asset?.audioUrls?.[audioIndex]?.path ?? asset?.url) : asset?.url;

  useEffect(() => {
    const media = ref.current;
    if (!media || !active) return;
    function sync() {
      const now = clock.getSnapshot();
      if (now < item.start || now >= item.start + item.duration) return;
      const time = item.source + Math.max(0, now - item.start);
      if (Number.isFinite(time) && Math.abs(media!.currentTime - time) > (playing ? .18 : .02)) {
        try { media!.currentTime = time; } catch { /* Metadata is still loading. */ }
      }
    }
    media.volume = Math.max(0, Math.min(1, item.gain));
    sync();
    return clock.subscribe(sync);
  }, [clock, playing, active, item.source, item.start, item.duration, item.gain, src]);

  useEffect(() => {
    const media = ref.current;
    wantsPlayback.current = playing && active;
    if (!media) return;
    if (!wantsPlayback.current) {
      if (!media.paused) media.pause();
      return;
    }
    let disposed = false;
    const fail = (reason: unknown) => {
      if (!disposed) onPlaybackError(`Не удалось воспроизвести «${asset?.name ?? 'медиа'}»: ${String(reason)}. Нажмите «Воспроизвести», чтобы повторить.`);
    };
    if (media.paused) {
      try {
        void media.play().then(() => {
          if (disposed && !wantsPlayback.current) media.pause();
        }, fail);
      } catch (reason) { fail(reason); }
    }
    return () => {
      disposed = true;
      wantsPlayback.current = false;
      media.pause();
    };
  }, [playing, active, src, onPlaybackError, asset?.name]);

  if (item.kind === 'audio') return <audio ref={element => { ref.current = element; }} src={src} preload="metadata" />;
  const style: CSSProperties = {
    left: `${item.x * 100}%`, top: `${item.y * 100}%`,
    width: `${item.width * 100}%`, height: `${item.height * 100}%`,
    opacity: item.opacity, display: active ? 'block' : 'none', objectFit: item.fit,
    filter: `brightness(${1 + item.brightness}) contrast(${item.contrast}) saturate(${item.saturation})`,
  };
  if (item.kind === 'text') {
    return <div className="ed-layer ed-text" style={{ ...style, color: item.color, fontSize: `${item.fontSize / canvasWidth * 100}cqw` }}>{item.text}</div>;
  }
  if (item.kind === 'image') return <img className="ed-layer" src={src} style={style} alt="" draggable={false} />;
  return <video className="ed-layer" ref={element => { ref.current = element; }} src={src} style={style} muted playsInline preload="metadata"
    onLoadedMetadata={() => { if (ref.current) ref.current.currentTime = item.source + Math.max(0, clock.getSnapshot() - item.start); }} />;
}
