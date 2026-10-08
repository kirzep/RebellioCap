import { describe, expect, it } from 'vitest';
import { normalizeError } from './host';

describe('memory admission errors', () => {
  it('explains Replay refusal and offers settings as a remedy', () => {
    const error = normalizeError({ code: 'replay.memory_budget_exceeded', message: 'Budget full' });
    expect(error.summary).toMatch(/дождитесь завершения сохранений/i);
    expect(error.actions).toContain('settings');
  });
  it('distinguishes a slow continuous writer from Replay refusal', () => {
    const error = normalizeError('recording.memory_budget_exceeded: Budget full');
    expect(error.summary).toMatch(/запись остановлена/i);
    expect(error.summary).toMatch(/диска/i);
  });
});


it('keeps RAM admission details visible and rollback diagnostics technical', () => {
  const error = normalizeError('config.replay_memory_unavailable: Допустимый лимит: 2048 МиБ. Уменьшите лимит или выберите «Авто».; rollback: Ok(()); previous engine: Ok(())');
  expect(error.code).toBe('config.replay_memory_unavailable');
  expect(error.summary).toContain('2048 МиБ');
  expect(error.summary).not.toContain('rollback');
  expect(error.technicalCause).toContain('rollback');
  expect(error.retryable).toBe(false);
  expect(error.actions).toContain('settings');
});
