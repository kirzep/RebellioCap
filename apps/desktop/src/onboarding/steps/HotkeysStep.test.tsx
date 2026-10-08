import React from 'react';
import { fireEvent, render, screen } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { createDraftDefaults } from '../../config/defaults';
import { HotkeysStep } from './HotkeysStep';

describe('hotkey capture cancellation', () => {
  it.each(['Escape', 'Tab'])('cancels with %s even when the WebView supplies no physical code', (key) => {
    const onChange = vi.fn();
    const onCaptureEnd = vi.fn();
    render(<HotkeysStep draft={createDraftDefaults()} onChange={onChange} onCaptureEnd={onCaptureEnd} />);
    fireEvent.click(screen.getByTestId('record-replay-hotkey-button'));
    fireEvent.keyDown(window, { key, code: '' });
    expect(onCaptureEnd).toHaveBeenCalledWith(false);
    expect(onChange).not.toHaveBeenCalled();
    expect(screen.queryByTestId('hotkey-capture-error')).not.toBeInTheDocument();
    expect(screen.getByTestId('record-replay-hotkey-button')).toHaveTextContent('Изменить');
  });
});
