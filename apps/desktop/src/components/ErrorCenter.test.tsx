import { cleanup, fireEvent, render, screen, within } from '@testing-library/react';
import { StrictMode } from 'react';
import { afterEach, expect, it, vi } from 'vitest';
import { ErrorCenterProvider } from './ErrorCenter';
import { ErrorNotice } from './Primitives';
afterEach(cleanup);

it('moves operational errors out of the form and preserves details and recovery actions', () => {
 const retry=vi.fn();
 render(<StrictMode><ErrorCenterProvider><section data-testid="form"><ErrorNotice title="Ошибка записи" technicalDetails="nvenc.failed" action={<button onClick={retry}>Повторить</button>}>Не удалось записать видео.</ErrorNotice></section></ErrorCenterProvider></StrictMode>);
 expect(within(screen.getByTestId('form')).queryByRole('alert')).toBeNull();
 const center=screen.getByRole('region',{name:'Ошибки приложения'});
 expect(within(center).getByText('Не удалось записать видео.')).toBeVisible();
 fireEvent.click(within(center).getByText('Технические подробности'));
 expect(within(center).getByText('nvenc.failed')).toBeVisible();
 fireEvent.click(within(center).getByRole('button',{name:'Повторить'}));expect(retry).toHaveBeenCalledOnce();
});

it('merges duplicates, keeps different errors accessible and removes resolved sources', () => {
 function View({duplicate=true,second=true}:{duplicate?:boolean;second?:boolean}){return <ErrorCenterProvider><ErrorNotice title="Запись">Ошибка A</ErrorNotice>{duplicate&&<ErrorNotice title="Запись">Ошибка A</ErrorNotice>}{second&&<ErrorNotice title="Логи">Ошибка B</ErrorNotice>}</ErrorCenterProvider>;}
 const view=render(<View/>);
 expect(screen.getAllByRole('alert')).toHaveLength(1);
 expect(screen.getByText('Ошибки: 2')).toBeInTheDocument();
 fireEvent.click(screen.getByRole('button',{name:'Предыдущая ошибка'}));expect(screen.getByText('Ошибка A')).toBeVisible();
 view.rerender(<View duplicate={false} second={false}/>);
 expect(screen.getByText('Ошибка A')).toBeVisible();expect(screen.queryByText('Ошибки: 2')).toBeNull();
});

it('dismisses without clearing the underlying state and shows a changed error', () => {
 function View({message}:{message:string}){return <ErrorCenterProvider><ErrorNotice title="Запись">{message}</ErrorNotice></ErrorCenterProvider>;}
 const view=render(<View message="Ошибка A"/>);
 fireEvent.click(screen.getByRole('button',{name:'Закрыть ошибку'}));expect(screen.queryByRole('alert')).toBeNull();
 view.rerender(<View message="Ошибка A"/>);expect(screen.queryByRole('alert')).toBeNull();
 view.rerender(<View message="Ошибка B"/>);expect(screen.getByText('Ошибка B')).toBeVisible();
});

it('keeps field validation inline and does not steal focus on error arrival', () => {
 const view=render(<ErrorCenterProvider><input aria-label="Размер"/><ErrorNotice inline tone="warning" title="Проверьте настройки">Ширина обязательна.</ErrorNotice></ErrorCenterProvider>);
 const input=screen.getByRole('textbox');input.focus();
 view.rerender(<ErrorCenterProvider><input aria-label="Размер"/><ErrorNotice inline tone="warning" title="Проверьте настройки">Ширина обязательна.</ErrorNotice><ErrorNotice title="Запись">Ошибка</ErrorNotice></ErrorCenterProvider>);
 expect(document.activeElement).toBe(input);expect(screen.getByText('Ширина обязательна.').closest('[data-error-center]')).toBeNull();
});

it('updates recovery callbacks even when the error text stays the same', () => {
 const first=vi.fn(),second=vi.fn();
 function View({retry}:{retry:()=>void}){return <ErrorCenterProvider><ErrorNotice title="Запись" action={<button onClick={retry}>Повторить</button>}>Ошибка A</ErrorNotice></ErrorCenterProvider>;}
 const view=render(<View retry={first}/>);view.rerender(<View retry={second}/>);
 fireEvent.click(screen.getByRole('button',{name:'Повторить'}));expect(first).not.toHaveBeenCalled();expect(second).toHaveBeenCalledOnce();
});

it('dismisses every duplicate source and reveals the remaining different error', () => {
 render(<ErrorCenterProvider><ErrorNotice title="Логи">Ошибка B</ErrorNotice><ErrorNotice title="Запись">Ошибка A</ErrorNotice><ErrorNotice title="Запись">Ошибка A</ErrorNotice></ErrorCenterProvider>);
 fireEvent.click(screen.getByRole('button',{name:'Предыдущая ошибка'}));
 fireEvent.click(screen.getByRole('button',{name:'Следующая ошибка'}));
 fireEvent.click(screen.getByRole('button',{name:'Закрыть ошибку'}));
 expect(screen.queryByText('Ошибка A')).toBeNull();expect(screen.getByText('Ошибка B')).toBeVisible();
 expect(screen.queryByLabelText('Следующая ошибка')).toBeNull();
});

it('routes recoverable operational warnings to the same card', () => {
 render(<ErrorCenterProvider><section data-testid="form"><ErrorNotice tone="warning" title="Список устройств">Не удалось обновить список.</ErrorNotice></section></ErrorCenterProvider>);
 expect(screen.getByTestId('form')).toBeEmptyDOMElement();
 expect(within(screen.getByRole('region',{name:'Ошибки приложения'})).getByText('Не удалось обновить список.')).toBeVisible();
});
