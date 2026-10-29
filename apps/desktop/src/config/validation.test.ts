import { describe, expect, it } from 'vitest';
import {
  areHotkeysEqual,
  isValidHotkey,
  isValidIdentity,
  isValidOutputPath,
  validateDraftStep,
} from './validation';

describe('config validation', () => {
  it('validates the CQP range including zero without requiring new fields in legacy profiles', () => {
    const video={monitor_id:'monitor',width:1920,height:1080,fps:60,bitrate:12000000};
    for(const cqp of [0,23,51]) expect(validateDraftStep(1,{...video,rate_control:'cqp',cqp})).toEqual([]);
    for(const cqp of [-1,52,23.5,null]) expect(validateDraftStep(1,{...video,rate_control:'cqp',cqp}).some(error=>error.field==='cqp')).toBe(true);
    expect(validateDraftStep(1,video)).toEqual([]);
  });
  it('requires a full explicit executable only in game mode', () => {
    const video = { monitor_id: 'monitor', width: 1920, height: 1080, fps: 60, bitrate: 12_000_000 };
    expect(validateDraftStep(1, { ...video, capture_mode: 'game' }).some(error => error.field === 'game_executable')).toBe(true);
    expect(validateDraftStep(1, { ...video, monitor_id: '', capture_mode: 'game', game_executable: 'C:\\Games\\game.exe' })).toEqual([]);
    for (const game_executable of ['game.exe', 'C:\\Games\\..\\game.exe', 'C:\\Games\\game.txt']) {
      expect(validateDraftStep(1, { ...video, capture_mode: 'game', game_executable }).some(error => error.field === 'game_executable')).toBe(true);
    }
    expect(validateDraftStep(1, video)).toEqual([]);
  });
  describe('isValidIdentity', () => {
    it('accepts valid ASCII/Unicode string within length', () => {
      expect(isValidIdentity('monitor-0')).toBe(true);
      expect(isValidIdentity('Микрофон Realtek')).toBe(true);
    });

    it('rejects empty or whitespace-only strings', () => {
      expect(isValidIdentity('')).toBe(false);
      expect(isValidIdentity('   ')).toBe(false);
      expect(isValidIdentity(null)).toBe(false);
    });

    it('rejects control characters', () => {
      expect(isValidIdentity('id\nwith\nnewlines')).toBe(false);
      expect(isValidIdentity('id\0null')).toBe(false);
    });
  });

  describe('isValidHotkey', () => {
    it('accepts standard function and letter keys', () => {
      expect(isValidHotkey({ key: 0x77, ctrl: false, alt: false, shift: false, win: false })).toBe(true); // F8
      expect(isValidHotkey({ key: 0x52, ctrl: true, alt: false, shift: true, win: false })).toBe(true); // Ctrl+Shift+R
    });

    it('rejects Windows key usage', () => {
      expect(isValidHotkey({ key: 0x52, ctrl: false, alt: false, shift: false, win: true })).toBe(false);
    });

    it('rejects Alt+Tab, Alt+Space, Alt+F4', () => {
      expect(isValidHotkey({ key: 0x09, ctrl: false, alt: true, shift: false, win: false })).toBe(false); // Alt+Tab
      expect(isValidHotkey({ key: 0x20, ctrl: false, alt: true, shift: false, win: false })).toBe(false); // Alt+Space
      expect(isValidHotkey({ key: 0x73, ctrl: false, alt: true, shift: false, win: false })).toBe(false); // Alt+F4
    });

    it('rejects Ctrl+Alt+Del and F12', () => {
      expect(isValidHotkey({ key: 0x2e, ctrl: true, alt: true, shift: false, win: false })).toBe(false); // Ctrl+Alt+Del
      expect(isValidHotkey({ key: 0x7b, ctrl: false, alt: false, shift: false, win: false })).toBe(false); // F12
    });
  });

  describe('areHotkeysEqual', () => {
    it('compares hotkeys accurately', () => {
      const h1 = { key: 0x77, ctrl: false, alt: false, shift: false, win: false };
      const h2 = { key: 0x77, ctrl: false, alt: false, shift: false, win: false };
      const h3 = { key: 0x77, ctrl: true, alt: false, shift: false, win: false };
      expect(areHotkeysEqual(h1, h2)).toBe(true);
      expect(areHotkeysEqual(h1, h3)).toBe(false);
    });
  });

  describe('isValidOutputPath', () => {
    it('accepts valid absolute paths', () => {
      expect(isValidOutputPath('C:/Users/User/Videos')).toBe(true);
      expect(isValidOutputPath('D:\\Clips')).toBe(true);
      expect(isValidOutputPath('\\\\server\\share\\clips')).toBe(true);
    });

    it('rejects relative paths or paths with traversal', () => {
      expect(isValidOutputPath('videos/clips')).toBe(false);
      expect(isValidOutputPath('/home/user/clips')).toBe(false);
      expect(isValidOutputPath('C:/Videos/../clips')).toBe(false);
      expect(isValidOutputPath('C:/Videos/./clips')).toBe(false);
    });
  });

  describe('validateDraftStep', () => {
    it('validates video step (step 1)', () => {
      expect(
        validateDraftStep(1, {
          monitor_id: 'mon-1',
          width: 1920,
          height: 1080,
          fps: 60,
          bitrate: 30000000,
        })
      ).toHaveLength(0);

      // Odd width/height rejected
      const errs = validateDraftStep(1, {
        monitor_id: 'mon-1',
        width: 1921,
        height: 1080,
        fps: 60,
        bitrate: 30000000,
      });
      expect(errs.some((e) => e.field === 'width')).toBe(true);
    });

    it('validates audio step (step 2)', () => {
      expect(
        validateDraftStep(2, {
          system_audio: { endpoint: 'sys-1' },
          microphone: 'disabled',
        })
      ).toHaveLength(0);

      const errs = validateDraftStep(2, {});
      expect(errs.some((e) => e.field === 'system_audio')).toBe(true);
      expect(errs.some((e) => e.field === 'microphone')).toBe(true);
    });

    it('validates replay/storage step (step 3)', () => {
      expect(
        validateDraftStep(3, {
          replay_seconds: 30,
          replay_mode: 'ram',
          container: 'mp4',
          output_directory: 'C:/clips',
        })
      ).toHaveLength(0);

      expect(
        validateDraftStep(3, {
          replay_seconds: 30,
          replay_mode: 'ram',
          container: 'mp4',
          output_directory: '\\\\?\\C:\\Users\\User\\Documents',
        })
      ).toHaveLength(0);

      const errs = validateDraftStep(3, {
        replay_seconds: 0,
        replay_mode: 'ram',
        container: 'mp4',
        output_directory: 'relative/path',
      });
      expect(errs.some((e) => e.field === 'replay_seconds')).toBe(true);
      expect(errs.some((e) => e.field === 'output_directory')).toBe(true);
    });

    it('validates hotkeys step and rejects duplicate hotkeys (step 4)', () => {
      const h = { key: 0x77, ctrl: false, alt: false, shift: false, win: false };
      const errs = validateDraftStep(4, {
        save_replay_hotkey: h,
        toggle_recording_hotkey: h,
      });
      expect(errs.some((e) => e.field === 'duplicate_hotkeys')).toBe(true);
    });
  });
});


it.each([null, -1, 1, 63, 8193, 64.5, NaN, Infinity])('rejects invalid Replay memory limit %s', value => {
  expect(validateDraftStep(3, {replay_memory_limit_mb:value}).some(error => error.field === 'replay_memory_limit_mb')).toBe(true);
});
it.each([undefined, 0, 64, 1024, 2048, 8192])('accepts Replay memory limit %s', value => {
  expect(validateDraftStep(3, {replay_memory_limit_mb:value}).some(error => error.field === 'replay_memory_limit_mb')).toBe(false);
});
