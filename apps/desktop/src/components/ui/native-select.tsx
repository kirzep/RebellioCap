import * as React from 'react';
import { cn } from 'cn';
import { Check, ChevronDown, ChevronUp } from 'lucide-react';
import { Select } from 'radix-ui';
import styles from './native-select.module.css';

type OptionProps = React.ComponentProps<'option'>;
const EMPTY_VALUE = '__empty_selection__';
function collectOptions(children: React.ReactNode): OptionProps[] {
  return React.Children.toArray(children).flatMap(child => {
    if (!React.isValidElement<OptionProps>(child)) return [];
    if (child.type === React.Fragment || child.type === NativeSelectOptGroup) return collectOptions(child.props.children);
    return [child.props];
  });
}

function NativeSelect({ className, size = 'default', children, onChange, id, value, defaultValue,
  disabled, renderOption, contentClassName, ...props }: Omit<React.ComponentProps<'select'>, 'size'> & { size?: 'sm' | 'default'; contentClassName?: string; renderOption?: (value: string, label: React.ReactNode) => React.ReactNode }) {
  const nativeRef = React.useRef<HTMLSelectElement>(null);
  const [localValue, setLocalValue] = React.useState(String(defaultValue ?? ''));
  const options = collectOptions(children);
  const selected = String(value ?? localValue);
  const selectedOption = options.find(option => String(option.value ?? '') === selected);
  function change(next: string) {
    const decoded = next === EMPTY_VALUE ? '' : next;
    setLocalValue(decoded);
    if (nativeRef.current) {
      nativeRef.current.value = decoded;
      onChange?.({ target: nativeRef.current, currentTarget: nativeRef.current } as React.ChangeEvent<HTMLSelectElement>);
    }
  }
  return <div data-slot="native-select-wrapper" className={styles.wrapper}>
    {/* Retain native form values and the existing change-event contract. */}
    <select {...props} ref={nativeRef} value={selected} disabled={disabled} onChange={event => {
      setLocalValue(event.target.value); onChange?.(event);
    }} hidden aria-hidden="true" tabIndex={-1}>{children}</select>
    <Select.Root value={selected || EMPTY_VALUE} onValueChange={change} disabled={disabled} required={props.required}>
      <Select.Trigger id={id} data-slot="native-select" data-size={size}
        className={cn(styles.trigger, className)} aria-label={props['aria-label']} aria-invalid={props['aria-invalid']} aria-describedby={props['aria-describedby']}>
        <Select.Value>{renderOption ? renderOption(selected, selectedOption?.children) : selectedOption?.children}</Select.Value><Select.Icon className={styles.icon}><ChevronDown size={16} /></Select.Icon>
      </Select.Trigger>
      <Select.Portal><Select.Content className={cn(styles.content, contentClassName)} position="popper" sideOffset={6} collisionPadding={12}>
        <Select.ScrollUpButton className={styles.scrollButton}><ChevronUp size={14} /></Select.ScrollUpButton>
        <Select.Viewport className={styles.viewport}>
          {options.map((option, index) => <Select.Item key={`${option.value}-${index}`} value={String(option.value ?? '') || EMPTY_VALUE} disabled={option.disabled} className={styles.option}>
            <Select.ItemIndicator className={styles.check}><Check size={14} /></Select.ItemIndicator><Select.ItemText>{renderOption ? renderOption(String(option.value ?? ''), option.children) : option.children}</Select.ItemText>
          </Select.Item>)}
        </Select.Viewport>
        <Select.ScrollDownButton className={styles.scrollButton}><ChevronDown size={14} /></Select.ScrollDownButton>
      </Select.Content></Select.Portal>
    </Select.Root>
  </div>;
}
function NativeSelectOption(props: OptionProps) { return <option {...props} />; }
function NativeSelectOptGroup(props: React.ComponentProps<'optgroup'>) { return <optgroup {...props} />; }
export { NativeSelect, NativeSelectOption, NativeSelectOptGroup };
