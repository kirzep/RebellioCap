import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { StrictMode } from 'react';
import { ClipPlayer } from './ClipPlayer';
import { applyLanguageSettings } from '../i18n';

it('translates canonical audio labels while retaining mixed-track ordering and user labels in English', () => {
  applyLanguageSettings({ preference: 'en', language: 'en' });
  render(<ClipPlayer name="Мой клип.mp4" media={{ id: 'english', video: 'video.mp4', tracks: [
    { path: 'mixed.m4a', label: 'Системный звук + микрофон', enabled: false },
    { path: 'mic.m4a', label: 'Микрофон', enabled: true },
    { path: 'custom.m4a', label: 'Моя дорожка', enabled: true },
  ] }} onClose={vi.fn()} />);
  fireEvent.click(screen.getByRole('button', { name: 'Audio and tracks' }));
  expect(screen.getByLabelText('Volume Microphone')).toBeVisible();
  expect(screen.getByText('Моя дорожка')).toBeVisible();
  expect(screen.getByRole('heading', { name: 'Мой клип.mp4' })).toBeVisible();
  const labels = document.querySelectorAll('[class*="channelName"] > span');
  expect(Array.from(labels, el => el.textContent)).toEqual(['Microphone', 'Моя дорожка', 'Combined track']);
});

beforeEach(() => {
  vi.spyOn(HTMLMediaElement.prototype, 'pause').mockImplementation(() => {});
  vi.spyOn(HTMLMediaElement.prototype, 'load').mockImplementation(() => {});
  vi.spyOn(HTMLMediaElement.prototype, 'play').mockResolvedValue();
});

afterEach(async () => {
  cleanup();
  // Player disposal is deferred to distinguish unmounting from StrictMode replay.
  await new Promise(resolve => setTimeout(resolve, 0));
  vi.restoreAllMocks();
});

it('plays isolated tracks with independent volumes and mixed audio disabled', () => {
  render(<ClipPlayer name="clip.mp4" media={{ id: '1', video: 'video.mp4', tracks: [
    { path: 'system.m4a', label: 'Системный звук', enabled: true },
    { path: 'mic.m4a', label: 'Микрофон', enabled: true },
    { path: 'mixed.m4a', label: 'Системный звук + микрофон', enabled: false },
  ] }} onClose={vi.fn()} />);
  const audio = document.querySelectorAll('audio');
  expect(audio[0].volume).toBe(1);
  expect(audio[1].volume).toBe(1);
  expect(audio[2].volume).toBe(0);
  fireEvent.click(screen.getByRole('button', { name: 'Звук и дорожки' }));
  fireEvent.change(screen.getByLabelText('Громкость Микрофон'), { target: { value: '35' } });
  expect(audio[1].volume).toBe(0.35);
  expect(audio[0].volume).toBe(1);
  const video = document.querySelector('video')!;
  video.currentTime = 12;
  fireEvent.seeking(video);
  expect(audio[0].currentTime).toBe(12);
  expect(audio[1].currentTime).toBe(12);
});

it('starts and pauses the tracks together with video', () => {
  render(<ClipPlayer name="clip.mp4" media={{ id: '2', video: 'video.mp4', tracks: [
    { path: 'mic.m4a', label: 'Микрофон', enabled: true },
    { path: 'system.m4a', label: 'Системный звук', enabled: true },
  ] }} onClose={vi.fn()} />);
  const video = document.querySelector('video')!;
  Object.defineProperty(video, 'paused', { value: false, configurable: true });
  fireEvent.play(video);
  expect(HTMLMediaElement.prototype.play).toHaveBeenCalledTimes(2);
  fireEvent.pause(video);
  expect(HTMLMediaElement.prototype.pause).toHaveBeenCalledTimes(2);
});

it('releases media only after closing, including under StrictMode', async () => {
  const release = vi.fn();
  const view = render(<StrictMode><ClipPlayer name="clip.mp4" media={{ id: '3', video: 'video.mp4', tracks: [] }} onClose={vi.fn()} onRelease={release} /></StrictMode>);
  await new Promise(resolve => setTimeout(resolve, 10));
  expect(release).not.toHaveBeenCalled();
  view.unmount();
  await waitFor(() => expect(release).toHaveBeenCalledExactlyOnceWith('3'));
});

it('uses custom playback controls and a seekable timeline', () => {
  render(<ClipPlayer name="clip.mp4" media={{ id: '4', video: 'video.mp4', tracks: [] }} onClose={vi.fn()} />);
  const video = document.querySelector('video')!;
  expect(video).not.toHaveAttribute('controls');
  Object.defineProperty(video, 'duration', { value: 90, configurable: true });
  video.currentTime = 10;
  fireEvent.loadedMetadata(video);
  fireEvent.timeUpdate(video);
  expect(screen.getByLabelText('Позиция воспроизведения')).toHaveAttribute('max', '90');
  fireEvent.click(screen.getByRole('button', { name: 'Воспроизвести' }));
  expect(HTMLMediaElement.prototype.play).toHaveBeenCalledTimes(1);
  fireEvent.change(screen.getByLabelText('Позиция воспроизведения'), { target: { value: '25' } });
  expect(video.currentTime).toBe(25);
  fireEvent.click(screen.getByRole('button', { name: 'Вперёд на 5 секунд' }));
  expect(video.currentTime).toBe(30);
  fireEvent.click(screen.getByRole('button', { name: 'Назад на 5 секунд' }));
  expect(video.currentTime).toBe(25);
});

it('toggles individual channels and restores their previous volume', () => {
  render(<ClipPlayer name="clip.mp4" media={{ id: '5', video: 'video.mp4', tracks: [
    { path: 'mic.m4a', label: 'Микрофон', enabled: true },
    { path: 'system.m4a', label: 'Системный звук', enabled: true },
  ] }} onClose={vi.fn()} />);
  fireEvent.click(screen.getByRole('button', { name: 'Звук и дорожки' }));
  fireEvent.change(screen.getByLabelText('Громкость Микрофон'), { target: { value: '35' } });
  fireEvent.click(screen.getByRole('button', { name: 'Отключить Микрофон' }));
  expect(document.querySelectorAll('audio')[0].volume).toBe(0);
  expect(document.querySelectorAll('audio')[1].volume).toBe(1);
  fireEvent.click(screen.getByRole('button', { name: 'Включить Микрофон' }));
  expect(document.querySelectorAll('audio')[0].volume).toBe(0.35);
});

it('keeps audio controls inside the video and shows them only on request', () => {
  render(<ClipPlayer name="clip.mp4" media={{ id: '6', video: 'video.mp4', tracks: [
    { path: 'mic.m4a', label: 'Микрофон', enabled: true },
  ] }} onClose={vi.fn()} />);
  const toggle = screen.getByRole('button', { name: 'Звук и дорожки' });
  expect(toggle).toHaveAttribute('aria-expanded', 'false');
  expect(screen.queryByRole('region', { name: 'Аудиомикшер' })).not.toBeInTheDocument();
  fireEvent.click(toggle);
  const mixer = screen.getByRole('region', { name: 'Аудиомикшер' });
  expect(mixer.parentElement).toContainElement(document.querySelector('video'));
  fireEvent.keyDown(mixer, { key: 'Escape' });
  expect(toggle).toHaveAttribute('aria-expanded', 'false');
  expect(screen.getByRole('dialog')).toBeInTheDocument();
});

it('pauses video and audio when the window is hidden and leaves playback paused on return', () => {
  render(<ClipPlayer name="clip.mp4" media={{ id: 'hidden', video: 'video.mp4', tracks: [
    { path: 'mic.m4a', label: 'Микрофон', enabled: true },
  ] }} onClose={vi.fn()} />);
  const video = document.querySelector('video')!;
  const audio = document.querySelector('audio')!;
  Object.defineProperty(video, 'paused', { value: false, configurable: true });
  fireEvent.play(video);
  const videoPause = vi.spyOn(video, 'pause');
  const audioPause = vi.spyOn(audio, 'pause');
  const visibility = vi.spyOn(document, 'visibilityState', 'get').mockReturnValue('hidden');
  fireEvent(document, new Event('visibilitychange'));
  expect(videoPause).toHaveBeenCalled();
  expect(audioPause).toHaveBeenCalled();
  expect(screen.getByRole('button', { name: 'Воспроизвести' })).toBeInTheDocument();
  vi.mocked(HTMLMediaElement.prototype.play).mockClear();
  visibility.mockReturnValue('visible');
  fireEvent(document, new Event('visibilitychange'));
  expect(HTMLMediaElement.prototype.play).not.toHaveBeenCalled();
});

it('only schedules audio synchronization while playing', () => {
  const schedule = vi.spyOn(window, 'setInterval');
  const cancel = vi.spyOn(window, 'clearInterval');
  render(<ClipPlayer name="clip.mp4" media={{ id: 'timers', video: 'video.mp4', tracks: [] }} onClose={vi.fn()} />);
  expect(schedule).not.toHaveBeenCalled();
  const video = document.querySelector('video')!;
  fireEvent.play(video);
  expect(schedule).toHaveBeenCalledWith(expect.any(Function), 100);
  const timer = schedule.mock.results.at(-1)!.value;
  fireEvent.pause(video);
  expect(cancel).toHaveBeenCalledWith(timer);
});

it('does not autoplay when preparation finishes after the window was hidden', () => {
  vi.spyOn(document, 'visibilityState', 'get').mockReturnValue('hidden');
  render(<ClipPlayer name="clip.mp4" media={{ id: 'hidden-mount', video: 'video.mp4', tracks: [
    { path: 'mic.m4a', label: 'Микрофон', enabled: true },
  ] }} onClose={vi.fn()} />);
  const video = document.querySelector('video')!;
  expect(video.autoplay).toBe(false);
  Object.defineProperty(video, 'paused', { value: false, configurable: true });
  const pause = vi.spyOn(video, 'pause');
  fireEvent.play(video);
  fireEvent.playing(video);
  expect(pause).toHaveBeenCalled();
  expect(HTMLMediaElement.prototype.play).not.toHaveBeenCalled();
  expect(screen.getByRole('button', { name: 'Воспроизвести' })).toBeInTheDocument();
});
