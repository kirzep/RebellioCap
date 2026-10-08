import { expect, it } from 'vitest';
import contract from '../../../../contracts/recording-settings.v1.json';
import { createDraftDefaults, DEFAULT_SAVE_REPLAY_HOTKEY, DEFAULT_TOGGLE_RECORDING_HOTKEY } from './defaults';
import { validateDraftStep } from './validation';

it('uses the versioned recording hotkey defaults', () => {
  expect(contract.version).toBe(1);
  expect(DEFAULT_SAVE_REPLAY_HOTKEY).toEqual(contract.defaults.save_replay_hotkey);
  expect(DEFAULT_TOGGLE_RECORDING_HOTKEY).toEqual(contract.defaults.toggle_recording_hotkey);
});

it.each(Object.keys(contract.ranges) as Array<keyof typeof contract.ranges>)('enforces the shared %s boundaries and message', field => {
  const { min, max } = contract.ranges[field];
  for (const [value, accepted] of [[min - 1, false], [min, true], [max, true], [max + 1, false]] as const) {
    const draft = { ...createDraftDefaults(), monitor_id: '1:0', width: 1920, height: 1080, fps: 60, bitrate: 30_000_000, output_directory: 'C:\\clips', [field]: value };
    const errors = validateDraftStep(field === 'replay_seconds' || field === 'replay_memory_limit_mb' ? 3 : 1, draft);
    const error = errors.find(error => error.field === field);
    expect(Boolean(error), `${field}=${value}`).toBe(!accepted);
    if (error) expect(error.message).toBe(contract.messages[field]);
  }
});
