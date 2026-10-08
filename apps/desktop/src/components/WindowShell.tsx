import { t, useTranslation } from '../i18n';
import { WindowTitlebar } from './WindowTitlebar';
import React, { useLayoutEffect, useRef, useState } from 'react';
import { ArrowLeft, PanelLeft, Settings2, Settings, Scissors } from 'lucide-react';
import { AppIcon, type AppIconName } from './AppIcon';
import { DeveloperBrand } from './DeveloperMode';
import { cn } from 'cn';
import { Button } from './ui/button';
import { Sidebar, SidebarProvider, SidebarContent, SidebarFooter,
  SidebarGroup, SidebarGroupContent, SidebarMenu, SidebarMenuItem, SidebarMenuButton } from './ui/sidebar';
import styles from './WindowShell.module.css';
import { UpdateNotice } from './UpdateNotice';
import type { AvailableUpdate, UpdateProgress } from '../bridge/contracts';

export interface WindowShellNavigationItem {
  id: string;
  label: string;
  disabled?: boolean;
  group?: string;
}
export interface WindowShellProps {
  children: React.ReactNode;
  productName?: string;
  context?: React.ReactNode;
  navigation?: readonly WindowShellNavigationItem[];
  settingsNavigation?: WindowShellNavigationItem;
  availableUpdate?: AvailableUpdate | null;
  onInstallUpdate?: (onProgress: (progress: UpdateProgress) => void) => Promise<void>;
  activeNavigation?: string;
  onNavigate?: (id: string) => void;
  contentWidth?: 'form' | 'setup' | 'wide';
  headerActions?: React.ReactNode;
  windowStatus?: React.ReactNode;
  footer?: React.ReactNode;
  onBack?: () => void;
  backLabel?: string;
  contentLabel?: string;
  className?: string;
  contentClassName?: string;
}
export function WindowShell({ children, navigation, settingsNavigation, availableUpdate, onInstallUpdate,
  activeNavigation, onNavigate, contentWidth, headerActions, windowStatus, footer, onBack, backLabel = 'Назад',
  contentLabel, className, contentClassName }: WindowShellProps) {
  useTranslation();
  const contentRef = useRef<HTMLElement>(null);
  const [sidebarCollapsed, setSidebarCollapsed] = useState(() => {
    try { return localStorage.getItem('rebelliocap.sidebar-collapsed') === 'true'; }
    catch { return false; }
  });
  function toggleSidebar() {
    const next = !sidebarCollapsed;
    setSidebarCollapsed(next);
    try { localStorage.setItem('rebelliocap.sidebar-collapsed', String(next)); }
    catch { /* The menu still works when storage is unavailable. */ }
  }
  const innerRef = useRef<HTMLDivElement>(null);
  const previousNavigation = useRef(activeNavigation);
  const keyboardNavigation = useRef(false);
  useLayoutEffect(() => {
    if (contentRef.current) contentRef.current.scrollTop = 0;
    const changed = previousNavigation.current !== activeNavigation;
    previousNavigation.current = activeNavigation;
    const skipMotion = keyboardNavigation.current;
    keyboardNavigation.current = false;
    if (!changed || skipMotion || window.matchMedia('(prefers-reduced-motion: reduce)').matches) return;
    const animation = (innerRef.current ?? contentRef.current)?.animate?.([
      { opacity: 0.6, transform: 'translateY(5px)' },
      { opacity: 1, transform: 'translateY(0)' },
    ], { duration: 170, easing: 'cubic-bezier(.22,1,.36,1)' });
    return () => animation?.cancel();
  }, [activeNavigation]);
  const hasNavigation = Boolean(navigation?.length);
  const titlebar = <WindowTitlebar>
    {hasNavigation && <Button variant="ghost" size="icon" type="button" className={styles.sidebarToggle}
      aria-label={sidebarCollapsed ? t('Развернуть боковое меню') : t('Свернуть боковое меню')}
      title={sidebarCollapsed ? t('Развернуть боковое меню') : t('Свернуть боковое меню')}
      aria-expanded={!sidebarCollapsed} aria-controls="workspace-navigation" onClick={toggleSidebar}>
      <PanelLeft aria-hidden="true" />
    </Button>}
    {windowStatus}
    {(onBack || headerActions) && <>
      {onBack && <Button variant="ghost" size="icon" className="size-10" type="button"
        aria-label={backLabel} title={backLabel} onClick={onBack} data-testid="shell-back-button">
        <ArrowLeft aria-hidden="true" />
      </Button>}
      {headerActions}
    </>}
  </WindowTitlebar>;
  const content = <>
    <main ref={contentRef} className={cn(styles.content, contentClassName)} aria-label={contentLabel}>
      {contentWidth ? <div ref={innerRef} className={cn(styles.contentInner, styles[`contentWidth${contentWidth}`])}>{children}</div> : children}
    </main>
    {footer && <footer className={styles.footer}>{footer}</footer>}
  </>;
  if (!hasNavigation) {
    return <div className={cn(styles.shell, className)} data-testid="window-shell">{titlebar}{content}</div>;
  }
  return (
    <div className={cn(styles.shell, className)} data-testid="window-shell">
      {titlebar}
    <SidebarProvider className={styles.workspace} enableKeyboardShortcut={false}
      data-sidebar-collapsed={sidebarCollapsed}
      style={{ '--sidebar-width': 'var(--app-sidebar-width)' } as React.CSSProperties}>
      <Sidebar collapsible="none" className={styles.sidebar}>
        <SidebarContent>
          <div className={styles.brand}><DeveloperBrand className={styles.brandLogo} /><span>RebellioCap</span></div>
          <SidebarGroup>
            <SidebarGroupContent>
              <nav id="workspace-navigation" aria-label={t("Разделы приложения")}>
                <SidebarMenu>
                  {navigation?.map((item) => {
                    const customIcon = ['recording', 'clips', 'video', 'audio', 'replay', 'hotkeys'].includes(item.id);
                    return <SidebarMenuItem key={item.id}>
                      {item.group && <p className={styles.navGroup}>{t(item.group)}</p>}
                      <SidebarMenuButton type="button" size="lg" className={styles.navButton}
                        isActive={item.id === activeNavigation}
                        aria-current={item.id === activeNavigation ? 'page' : undefined}
                        aria-label={t(item.label)} title={t(item.label)}
                        disabled={item.disabled || !onNavigate} onClick={(event) => {
                          keyboardNavigation.current = event.detail === 0;
                          onNavigate?.(item.id);
                        }}>
                        {item.id === 'editor' ? <Scissors aria-hidden="true" /> : customIcon ? <AppIcon name={item.id as AppIconName} /> : <Settings2 aria-hidden="true" />}<span className={styles.navLabel}>{t(item.label)}</span>
                      </SidebarMenuButton>
                    </SidebarMenuItem>;
                  })}
                </SidebarMenu>
              </nav>
            </SidebarGroupContent>
          </SidebarGroup>
        </SidebarContent>
        {settingsNavigation && <SidebarFooter className={styles.settingsFooter}>
          <SidebarMenu>
            {availableUpdate && onInstallUpdate && <UpdateNotice update={availableUpdate} onInstall={onInstallUpdate} disabled={settingsNavigation.disabled} />}
            <SidebarMenuItem>
            <SidebarMenuButton type="button" size="lg" className={cn(styles.navButton, styles.settingsButton)}
              isActive={settingsNavigation.id === activeNavigation}
              aria-current={settingsNavigation.id === activeNavigation ? 'page' : undefined}
              aria-label={t(settingsNavigation.label)} title={t(settingsNavigation.label)}
              disabled={settingsNavigation.disabled || !onNavigate}
              onClick={event => {
                keyboardNavigation.current = event.detail === 0;
                onNavigate?.(settingsNavigation.id);
              }}>
              <Settings aria-hidden="true" /><span className={styles.navLabel}>{t(settingsNavigation.label)}</span>
            </SidebarMenuButton>
          </SidebarMenuItem></SidebarMenu>
        </SidebarFooter>}
      </Sidebar>
      <div className={styles.mainColumn}>{content}</div>
    </SidebarProvider>
    </div>
  );
}
