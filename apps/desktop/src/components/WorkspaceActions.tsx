import { createContext, useContext, useState, type ReactNode } from 'react';
import { createPortal } from 'react-dom';
import styles from './WorkspaceActions.module.css';

const ActionsTarget = createContext<HTMLDivElement | null | undefined>(undefined);

export function WorkspaceActionsFrame({ children }: { children: ReactNode }) {
  const [target, setTarget] = useState<HTMLDivElement | null>(null);
  return <ActionsTarget.Provider value={target}>
    {children}
    <div ref={setTarget} className={styles.host} data-testid="workspace-actions" />
  </ActionsTarget.Provider>;
}

export function WorkspaceActions({ children }: { children: ReactNode }) {
  const target = useContext(ActionsTarget);
  // Standalone settings retain their controls without requiring a shell.
  if (target === undefined) return children;
  return target ? createPortal(children, target) : null;
}
