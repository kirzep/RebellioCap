import audio from '../assets/icons/audio.svg';
import video from '../assets/icons/video.svg';
import replay from '../assets/icons/replay.svg';
import hotkeys from '../assets/icons/hotkeys.svg';
import clips from '../assets/icons/clips.svg';
import recording from '../assets/icons/recording.svg';

const icons = { audio, video, replay, hotkeys, clips, recording };
export type AppIconName = keyof typeof icons;

export function AppIcon({ name, size = 16 }: { name: AppIconName; size?: number }) {
  return <span aria-hidden="true" style={{ display: 'inline-block', flexShrink: 0,
    width: size, height: size, backgroundColor: 'currentColor',
    mask: `url("${icons[name]}") center / contain no-repeat`,
    WebkitMask: `url("${icons[name]}") center / contain no-repeat`,
  }} />;
}
