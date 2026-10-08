import {useCallback, useSyncExternalStore, type ReactNode} from 'react';

/** One editor's clock. Project/history remain React state in the editor root. */
export class PlaybackClock {
  private time = 0;
  private readonly listeners = new Set<() => void>();

  readonly getSnapshot = () => this.time;
  readonly subscribe = (listener: () => void) => {
    this.listeners.add(listener);
    return () => { this.listeners.delete(listener); };
  };
  readonly setTime = (update: number | ((time: number) => number)) => {
    const next = typeof update === 'function' ? update(this.time) : update;
    if (!Number.isFinite(next) || Object.is(next, this.time)) return;
    this.time = next;
    for (const listener of this.listeners) listener();
  };
}

/** Only these small dynamic regions subscribe to each playback tick. */
export function PlaybackTime({clock, children}: {
  clock: PlaybackClock;
  children: (time: number) => ReactNode;
}) {
  const time = useSyncExternalStore(clock.subscribe, clock.getSnapshot);
  return children(time);
}

/** A layer only rerenders when it enters or leaves its interval. */
export function PlaybackActive({clock, start, duration, children}: {
  clock: PlaybackClock;
  start: number;
  duration: number;
  children: (active: boolean) => ReactNode;
}) {
  const getActive = useCallback(() => {
    const time = clock.getSnapshot();
    return time >= start && time < start + duration;
  }, [clock, start, duration]);
  const active = useSyncExternalStore(clock.subscribe, getActive);
  return children(active);
}
