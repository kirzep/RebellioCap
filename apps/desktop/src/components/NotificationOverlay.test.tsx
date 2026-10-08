import { act, cleanup, render, screen } from '@testing-library/react';
import { StrictMode } from 'react';
import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { invoke } from '@tauri-apps/api/core';
import { NotificationOverlay } from './NotificationOverlay';
import { playNotificationSound } from './notificationSound';
import { getCurrentWebviewWindow } from '@tauri-apps/api/webviewWindow';
const nativeEvents = vi.hoisted(() => ({ ready: undefined as (() => void) | undefined, unlisten: vi.fn(), listen: vi.fn() }));
vi.mock('@tauri-apps/api/webviewWindow', () => ({ getCurrentWebviewWindow: () => ({
  listen: nativeEvents.listen,
}) }));
vi.mock('@tauri-apps/api/core', () => ({ invoke: vi.fn() }));
vi.mock('./notificationSound',async importOriginal=>({...await importOriginal<typeof import('./notificationSound')>(),playNotificationSound:vi.fn().mockResolvedValue(undefined)}));
beforeEach(()=>{nativeEvents.ready=undefined; nativeEvents.listen.mockImplementation(async (_event:string,callback:()=>void)=>{nativeEvents.ready=callback;return nativeEvents.unlisten;}); vi.mocked(playNotificationSound).mockResolvedValue(undefined);});
afterEach(() => { cleanup(); vi.useRealTimers(); vi.resetAllMocks(); });
it('drains once on subscription and stays idle until the native ready event', async () => {
  vi.useFakeTimers();
  vi.mocked(invoke).mockResolvedValue({events:[],settings:{overlayEnabled:true,soundEnabled:false,sound:'glass',volume:35}});
  const view=render(<NotificationOverlay/>);
  await act(async()=>{await vi.advanceTimersByTimeAsync(10000);});
  expect(invoke).toHaveBeenCalledTimes(1);
  vi.mocked(invoke).mockResolvedValueOnce({events:['replay_saved'],settings:{overlayEnabled:true,soundEnabled:false,sound:'glass',volume:35}});
  await act(async()=>{nativeEvents.ready?.(); await vi.advanceTimersByTimeAsync(0);});
  expect(screen.getByText('Повтор сохранён')).toBeInTheDocument();
  expect(invoke).toHaveBeenCalledTimes(2);
  view.unmount();
  expect(nativeEvents.unlisten).toHaveBeenCalledOnce();
});
it('coalesces readiness during an in-flight drain without overlapping or losing events',async()=>{
  vi.useFakeTimers();
  const settings={overlayEnabled:true,soundEnabled:false,sound:'glass',volume:35};
  let resolveBatch!: (batch:unknown)=>void;
  vi.mocked(invoke).mockImplementationOnce(()=>new Promise(resolve=>{resolveBatch=resolve;}));
  vi.mocked(invoke).mockResolvedValue({events:['replay_saved'],settings});
  render(<NotificationOverlay/>);
  await act(async()=>{await vi.advanceTimersByTimeAsync(10);});
  await act(async()=>{nativeEvents.ready?.();nativeEvents.ready?.();await vi.advanceTimersByTimeAsync(2000);});
  expect(invoke).toHaveBeenCalledTimes(1);
  await act(async()=>{resolveBatch({events:[],settings});await vi.advanceTimersByTimeAsync(10);});
  expect(invoke).toHaveBeenCalledTimes(2);
  expect(screen.getByText('Повтор сохранён')).toBeInTheDocument();
});
it('retries a failed drain and returns to event-driven idle after recovery',async()=>{
  vi.useFakeTimers();
  vi.mocked(invoke).mockRejectedValueOnce(new Error('IPC unavailable'));
  vi.mocked(invoke).mockResolvedValue({events:['replay_saved'],settings:{overlayEnabled:true,soundEnabled:false,sound:'glass',volume:35}});
  render(<NotificationOverlay/>);
  await act(async()=>{await vi.advanceTimersByTimeAsync(1500);});
  expect(screen.getByText('Повтор сохранён')).toBeInTheDocument();
  expect(invoke).toHaveBeenCalledTimes(2);
  await act(async()=>{await vi.advanceTimersByTimeAsync(10000);});
  expect(invoke).toHaveBeenCalledTimes(2);
});
it('retries subscription failures and retains readiness while registration is pending',async()=>{
  vi.useFakeTimers();
  nativeEvents.listen.mockRejectedValueOnce(new Error('not ready'));
  let register!: (release:()=>void)=>void;
  nativeEvents.listen.mockImplementationOnce(async (_event:string,callback:()=>void)=>{
    nativeEvents.ready=callback;
    return new Promise(resolve=>{register=resolve;});
  });
  vi.mocked(invoke).mockResolvedValue({events:['replay_saved'],settings:{overlayEnabled:true,soundEnabled:false,sound:'glass',volume:35}});
  render(<NotificationOverlay/>);
  await act(async()=>{await vi.advanceTimersByTimeAsync(1100);nativeEvents.ready?.();await vi.advanceTimersByTimeAsync(100);});
  expect(invoke).not.toHaveBeenCalled();
  await act(async()=>{register(nativeEvents.unlisten);await vi.advanceTimersByTimeAsync(10);});
  expect(invoke).toHaveBeenCalledTimes(1);
  expect(screen.getByText('Повтор сохранён')).toBeInTheDocument();
});
it('does not consume notifications in StrictMode discarded effects and removes a late subscription',async()=>{
  vi.useFakeTimers();
  vi.mocked(invoke).mockResolvedValue({events:[],settings:{overlayEnabled:true,soundEnabled:false,sound:'glass',volume:35}});
  const strict=render(<StrictMode><NotificationOverlay/></StrictMode>);
  await act(async()=>{await vi.advanceTimersByTimeAsync(100);});
  expect(invoke).toHaveBeenCalledTimes(1);
  strict.unmount();
  const oldReady=nativeEvents.ready;
  await act(async()=>{oldReady?.();await vi.advanceTimersByTimeAsync(100);});
  expect(invoke).toHaveBeenCalledTimes(1);
  let resolveListener!: (unlisten:()=>void)=>void;
  vi.mocked(getCurrentWebviewWindow().listen).mockImplementationOnce(()=>new Promise(resolve=>{resolveListener=resolve;}));
  const late=render(<NotificationOverlay/>);
  await act(async()=>{await vi.advanceTimersByTimeAsync(100);});
  late.unmount();
  const release=vi.fn();
  await act(async()=>{resolveListener(release);await vi.advanceTimersByTimeAsync(100);});
  expect(release).toHaveBeenCalledOnce();
  expect(invoke).toHaveBeenCalledTimes(1);
});
it('queues confirmed events, shows each once and clears the overlay', async () => {
  vi.useFakeTimers();
  vi.mocked(invoke).mockResolvedValue({ events: [], settings: { overlayEnabled: true, soundEnabled: false, sound: 'glass', volume: 35 } });
  vi.mocked(invoke).mockResolvedValueOnce({ events: ['recording_started', 'replay_saved'], settings: { overlayEnabled: true, soundEnabled: false, sound: 'glass', volume: 35 } });
  render(<NotificationOverlay />);
  await act(async () => { await vi.advanceTimersByTimeAsync(150); });
  expect(screen.getByText('Запись началась')).toBeInTheDocument();
  expect(screen.queryByText('Повтор сохранён')).not.toBeInTheDocument();
  await act(async () => { await vi.advanceTimersByTimeAsync(3600); });
  expect(screen.getByText('Повтор сохранён')).toBeInTheDocument();
  await act(async () => { await vi.advanceTimersByTimeAsync(3600); });
  expect(screen.queryByRole('status')).not.toBeInTheDocument();
});

it.each([[false,false],[false,true],[true,false],[true,true]])('handles overlay=%s and sound=%s independently',async(overlayEnabled,soundEnabled)=>{
  vi.useFakeTimers();
  const settings={overlayEnabled,soundEnabled,sound:'glass',volume:35};
  vi.mocked(invoke).mockResolvedValue({events:[],settings});
  vi.mocked(invoke).mockResolvedValueOnce({events:['replay_saved'],settings});
  render(<NotificationOverlay/>);
  await act(async()=>{await vi.advanceTimersByTimeAsync(150);});
  expect(screen.queryByRole('status')!==null).toBe(overlayEnabled);
  expect(playNotificationSound).toHaveBeenCalledTimes(soundEnabled?1:0);
  await act(async()=>{await vi.advanceTimersByTimeAsync(4000);});
  expect(screen.queryByRole('status')).not.toBeInTheDocument();
  expect(playNotificationSound).toHaveBeenCalledTimes(soundEnabled?1:0);
});

it('hides current visuals when overlay is disabled without repeating the sound',async()=>{
  vi.useFakeTimers();
  const visible={overlayEnabled:true,soundEnabled:true,sound:'glass',volume:35};
  vi.mocked(invoke).mockResolvedValue({events:[],settings:visible});
  vi.mocked(invoke).mockResolvedValueOnce({events:['replay_saved'],settings:visible});
  render(<NotificationOverlay/>);
  await act(async()=>{await vi.advanceTimersByTimeAsync(150);});
  expect(screen.getByRole('status')).toBeInTheDocument();
  vi.mocked(invoke).mockResolvedValue({events:[],settings:{...visible,overlayEnabled:false}});
  await act(async()=>{nativeEvents.ready?.(); await vi.advanceTimersByTimeAsync(150);});
  expect(screen.queryByRole('status')).not.toBeInTheDocument();
  expect(playNotificationSound).toHaveBeenCalledOnce();
});

it('discards queued events when both channels are disabled and does not revive them',async()=>{
  vi.useFakeTimers();
  const visible={overlayEnabled:true,soundEnabled:true,sound:'glass',volume:35};
  vi.mocked(invoke).mockResolvedValue({events:[],settings:visible});
  vi.mocked(invoke).mockResolvedValueOnce({events:['recording_started','replay_saved'],settings:visible});
  render(<NotificationOverlay/>);
  await act(async()=>{await vi.advanceTimersByTimeAsync(150);});
  vi.mocked(invoke).mockResolvedValue({events:[],settings:{...visible,overlayEnabled:false,soundEnabled:false}});
  await act(async()=>{nativeEvents.ready?.(); await vi.advanceTimersByTimeAsync(150);});
  expect(screen.queryByRole('status')).not.toBeInTheDocument();
  vi.mocked(invoke).mockResolvedValue({events:[],settings:visible});
  await act(async()=>{nativeEvents.ready?.(); await vi.advanceTimersByTimeAsync(8000);});
  expect(screen.queryByRole('status')).not.toBeInTheDocument();
  expect(playNotificationSound).toHaveBeenCalledOnce();
});

it('plays each sound-only event once while keeping the overlay empty',async()=>{
  vi.useFakeTimers();
  const settings={overlayEnabled:false,soundEnabled:true,sound:'soft',volume:60};
  vi.mocked(invoke).mockResolvedValue({events:[],settings});
  vi.mocked(invoke).mockResolvedValueOnce({events:['recording_started','replay_saved'],settings});
  render(<NotificationOverlay/>);
  await act(async()=>{await vi.advanceTimersByTimeAsync(150);});
  expect(playNotificationSound).toHaveBeenCalledTimes(1);
  expect(playNotificationSound).toHaveBeenLastCalledWith(settings);
  expect(screen.queryByRole('status')).not.toBeInTheDocument();
  await act(async()=>{await vi.advanceTimersByTimeAsync(3600);});
  expect(playNotificationSound).toHaveBeenCalledTimes(2);
  expect(screen.queryByRole('status')).not.toBeInTheDocument();
  await act(async()=>{await vi.advanceTimersByTimeAsync(3600);});
  expect(playNotificationSound).toHaveBeenCalledTimes(2);
});

it('uses current sound preferences for queued events while keeping visuals enabled',async()=>{
  vi.useFakeTimers();
  const settings={overlayEnabled:true,soundEnabled:true,sound:'glass',volume:35};
  vi.mocked(invoke).mockResolvedValue({events:[],settings});
  vi.mocked(invoke).mockResolvedValueOnce({events:['recording_started','replay_saved'],settings});
  render(<NotificationOverlay/>);
  await act(async()=>{await vi.advanceTimersByTimeAsync(150);});
  vi.mocked(invoke).mockResolvedValue({events:[],settings:{...settings,soundEnabled:false}});
  await act(async()=>{nativeEvents.ready?.(); await vi.advanceTimersByTimeAsync(3600);});
  expect(screen.getByText('Повтор сохранён')).toBeInTheDocument();
  expect(playNotificationSound).toHaveBeenCalledOnce();
});
