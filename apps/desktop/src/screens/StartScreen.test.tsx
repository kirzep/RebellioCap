import { fireEvent, render, screen } from '@testing-library/react';
import React from 'react';
import { describe, expect, it, vi } from 'vitest';
import { StartScreen } from './StartScreen';

describe('StartScreen', () => {
  it('renders start button and triggers onStart callback', () => {
    const onStart = vi.fn();
    render(<StartScreen onStart={onStart} />);

    const button = screen.getByTestId('start-button');
    expect(button).toBeInTheDocument();
    expect(button).toHaveTextContent('Начать');

    fireEvent.click(button);
    expect(onStart).toHaveBeenCalledTimes(1);
  });
});
