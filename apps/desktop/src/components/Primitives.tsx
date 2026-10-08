import { t, useTranslation } from '../i18n';
import React, { useEffect, useId, useRef } from 'react';
import { cn } from 'cn';
import { Button as ShadcnButton } from './ui/button';
import { Spinner } from './ui/spinner';
import { Alert, AlertTitle, AlertDescription } from './ui/alert';
import { Field, FieldLabel, FieldDescription, FieldError, FieldSet, FieldLegend } from './ui/field';
import { Dialog, DialogContent, DialogHeader, DialogTitle, DialogDescription, DialogFooter } from './ui/dialog';
import styles from './Primitives.module.css';

function translatedNode(value: React.ReactNode): React.ReactNode {
  return typeof value === 'string' ? t(value) : value;
}

export type ButtonVariant = 'primary' | 'secondary' | 'tertiary' | 'danger';
export type ButtonSize = 'default' | 'compact' | 'icon';
export interface ButtonProps extends React.ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: ButtonVariant;
  size?: ButtonSize;
  busy?: boolean;
  busyLabel?: React.ReactNode;
  leadingIcon?: React.ReactNode;
  trailingIcon?: React.ReactNode;
  fullWidth?: boolean;
}

const buttonVariants = { primary: 'default', secondary: 'outline', tertiary: 'ghost', danger: 'destructive' } as const;

/** Keep command/busy semantics at the app boundary; shadcn owns presentation. */
export const Button = React.forwardRef<HTMLButtonElement, ButtonProps>(function Button(
  { children, className, variant = 'secondary', size = 'default', busy = false,
    busyLabel, leadingIcon, trailingIcon, fullWidth = false, disabled, type = 'button', ...props }, ref
) {
  useTranslation();
  return (
    <ShadcnButton
      ref={ref} {...props} type={type}
      variant={buttonVariants[variant]} size={size === 'compact' ? 'sm' : size}
      className={cn(styles.button, 'min-h-10 h-auto min-w-0 whitespace-normal', fullWidth && 'w-full', className)}
      disabled={disabled || busy} aria-busy={busy || undefined}
    >
      {busy ? <Spinner aria-hidden="true" data-icon="inline-start" /> : leadingIcon && <span data-icon="inline-start" aria-hidden="true">{leadingIcon}</span>}
      {size === 'icon' ? (!busy && children) : <span className="min-w-0 break-words">{translatedNode(busy ? busyLabel ?? children : children)}</span>}
      {!busy && trailingIcon && size !== 'icon' && <span data-icon="inline-end" aria-hidden="true">{trailingIcon}</span>}
    </ShadcnButton>
  );
});

interface FieldControlAccessibilityProps {
  id?: string;
  required?: boolean;
  'aria-describedby'?: string;
  'aria-errormessage'?: string;
  'aria-invalid'?: boolean | 'true' | 'false';
}
export interface FormFieldProps {
  id: string;
  label: React.ReactNode;
  children: React.ReactNode;
  hint?: React.ReactNode;
  error?: React.ReactNode;
  required?: boolean;
  group?: boolean;
  className?: string;
  controlClassName?: string;
  labelAction?: React.ReactNode;
}
function joinIds(...ids: Array<string | undefined>): string | undefined {
  return ids.filter(Boolean).join(' ') || undefined;
}
export function FormField({ id, label, children, hint, error, required = false, group = false,
  className, controlClassName, labelAction }: FormFieldProps) {
  useTranslation();
  const hintId = hint ? `${id}-hint` : undefined;
  const errorId = error ? `${id}-error` : undefined;
  const labelContent = <>{translatedNode(label)}{required && <><span aria-hidden="true"> *</span><span className="sr-only">{t("Обязательное поле")}</span></>}</>;
  const messages = <>
    {hint && <FieldDescription id={hintId}>{translatedNode(hint)}</FieldDescription>}
    {error && <FieldError id={errorId}>{translatedNode(error)}</FieldError>}
  </>;
  if (group) {
    return (
      <FieldSet className={cn('min-w-0 gap-3', className)} data-invalid={Boolean(error)}
        aria-describedby={joinIds(hintId, errorId)} aria-invalid={error ? true : undefined}>
        <FieldLegend variant="label">{labelContent}</FieldLegend>
        {labelAction}
        <div className={controlClassName}>{children}</div>
        {messages}
      </FieldSet>
    );
  }
  let control = children;
  if (React.isValidElement<FieldControlAccessibilityProps>(children)) {
    control = React.cloneElement(children, {
      id, required: children.props.required ?? required,
      'aria-describedby': joinIds(children.props['aria-describedby'], hintId, errorId),
      'aria-errormessage': errorId ?? children.props['aria-errormessage'],
      'aria-invalid': error ? true : children.props['aria-invalid'],
    });
  }
  return (
    <Field className={cn('min-w-0', className)} data-invalid={Boolean(error)}>
      <div className="flex flex-wrap items-center justify-between gap-2">
        <FieldLabel htmlFor={id}>{labelContent}</FieldLabel>{labelAction}
      </div>
      <div className={controlClassName}>{control}</div>
      {messages}
    </Field>
  );
}

export type InlineStatusTone = 'neutral' | 'success' | 'warning' | 'danger' | 'busy';
export interface InlineStatusProps extends React.HTMLAttributes<HTMLDivElement> {
  tone?: InlineStatusTone;
  live?: 'off' | 'polite' | 'assertive';
}
export function InlineStatus({ children, className, tone = 'neutral', live = 'polite', ...props }: InlineStatusProps) {
  useTranslation();
  return (
    <div {...props} className={cn(styles.inlineStatus, styles[`status${tone[0].toUpperCase()}${tone.slice(1)}`], className)}
      role={props.role ?? 'status'} aria-live={live} aria-atomic="true" aria-busy={tone === 'busy' || undefined}>
      {tone === 'busy' ? <Spinner aria-hidden="true" /> : <span className={styles.statusMarker} aria-hidden="true" />}
      <span>{translatedNode(children)}</span>
    </div>
  );
}

export type ErrorNoticeTone = 'danger' | 'warning' | 'neutral';
export interface ErrorNoticeProps extends Omit<React.HTMLAttributes<HTMLDivElement>, 'title'> {
  title: React.ReactNode;
  tone?: ErrorNoticeTone;
  action?: React.ReactNode;
  technicalDetails?: React.ReactNode;
  technicalLabel?: string;
}
export function ErrorNotice({ title, children, tone = 'danger', action, technicalDetails,
  technicalLabel = 'Технические подробности', className, ...props }: ErrorNoticeProps) {
  useTranslation();
  return (
    <Alert {...props} className={cn(styles.errorNotice, styles[`notice${tone[0].toUpperCase()}${tone.slice(1)}`], className)} variant={tone === 'danger' ? 'destructive' : 'default'}
      role={props.role ?? (tone === 'danger' ? 'alert' : 'status')}>
      <AlertTitle className="line-clamp-none">{translatedNode(title)}</AlertTitle>
      <AlertDescription className="min-w-0 gap-3">
        {children && <div className="min-w-0 break-words">{translatedNode(children)}</div>}
        {action}
        {technicalDetails && <details className="w-full min-w-0">
          <summary>{t(technicalLabel)}</summary>
          <div className="whitespace-pre-wrap break-words font-mono text-xs">{technicalDetails}</div>
        </details>}
      </AlertDescription>
    </Alert>
  );
}

export interface ConfirmDialogProps {
  open: boolean;
  title: React.ReactNode;
  description?: React.ReactNode;
  children?: React.ReactNode;
  confirmLabel: React.ReactNode;
  cancelLabel?: React.ReactNode;
  destructive?: boolean;
  busy?: boolean;
  busyLabel?: React.ReactNode;
  initialFocus?: 'confirm' | 'cancel';
  closeOnBackdrop?: boolean;
  onConfirm: () => void;
  onCancel: () => void;
}
export function ConfirmDialog({ open, title, description, children, confirmLabel, cancelLabel = 'Отмена',
  destructive = false, busy = false, busyLabel, initialFocus = 'cancel', closeOnBackdrop = false,
  onConfirm, onCancel }: ConfirmDialogProps) {
  const confirmRef = useRef<HTMLButtonElement>(null);
  const cancelRef = useRef<HTMLButtonElement>(null);
  const returnFocusRef = useRef<HTMLElement | null>(null);
  const contentRef = useRef<HTMLDivElement>(null);
  const descriptionId = useId();
  useEffect(() => {
    if (open && busy && document.activeElement instanceof HTMLElement &&
        document.activeElement.matches(':disabled')) contentRef.current?.focus();
  }, [open, busy]);
  return (
    <Dialog open={open} onOpenChange={(next) => { if (!next && !busy) onCancel(); }}>
      <DialogContent ref={contentRef} tabIndex={-1} showCloseButton={false} className="max-h-[calc(100dvh-2rem)] overflow-y-auto"
        aria-busy={busy || undefined} aria-describedby={description ? descriptionId : undefined}
        onOpenAutoFocus={(event) => {
          event.preventDefault();
          returnFocusRef.current = document.activeElement instanceof HTMLElement ? document.activeElement : null;
          (busy ? contentRef.current : initialFocus === 'confirm' ? confirmRef.current : cancelRef.current)?.focus();
        }}
        onCloseAutoFocus={(event) => {
          event.preventDefault();
          if (returnFocusRef.current?.isConnected) returnFocusRef.current.focus();
        }}
        onEscapeKeyDown={(event) => { if (busy) event.preventDefault(); }}
        onInteractOutside={(event) => { if (busy || !closeOnBackdrop) event.preventDefault(); }}>
        <DialogHeader>
          <DialogTitle>{translatedNode(title)}</DialogTitle>
          {description && <DialogDescription id={descriptionId} asChild><div>{translatedNode(description)}</div></DialogDescription>}
        </DialogHeader>
        {children && <div className="min-w-0">{children}</div>}
        <DialogFooter>
          <Button ref={cancelRef} disabled={busy} onClick={onCancel}>{cancelLabel}</Button>
          <Button ref={confirmRef} variant={destructive ? 'danger' : 'primary'} busy={busy}
            busyLabel={busyLabel} onClick={onConfirm}>{confirmLabel}</Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}
