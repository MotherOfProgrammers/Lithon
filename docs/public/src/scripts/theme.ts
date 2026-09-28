export type Theme = "light" | "dark";

export const THEME_STORAGE_KEY = "lithon-theme";

const DARK_SCHEME_QUERY = "(prefers-color-scheme: dark)";

const THEME_COLORS: Readonly<Record<Theme, string>> = {
  light: "#f8fff4",
  dark: "#474350",
};

function isTheme(value: unknown): value is Theme {
  return value === "light" || value === "dark";
}

function readStorage(key: string): string | null {
  try {
    return window.localStorage.getItem(key);
  } catch {
    return null;
  }
}

function writeStorage(key: string, value: string): void {
  try {
    window.localStorage.setItem(key, value);
  } catch {}
}

export function systemTheme(): Theme {
  return window.matchMedia(DARK_SCHEME_QUERY).matches ? "dark" : "light";
}

export function storedTheme(): Theme | null {
  const stored = readStorage(THEME_STORAGE_KEY);
  return isTheme(stored) ? stored : null;
}

export function currentTheme(): Theme {
  const applied = document.documentElement.dataset["theme"];
  return isTheme(applied) ? applied : systemTheme();
}

function syncThemeColor(theme: Theme): void {
  const meta = document.querySelector<HTMLMetaElement>('meta[name="theme-color"]');
  if (meta) meta.content = THEME_COLORS[theme];
}

export function applyTheme(theme: Theme, persist: boolean): void {
  document.documentElement.dataset["theme"] = theme;
  if (persist) writeStorage(THEME_STORAGE_KEY, theme);
  syncThemeColor(theme);
}

function updateToggleLabel(toggle: HTMLElement, theme: Theme): void {
  toggle.setAttribute("aria-label", theme === "dark" ? "Switch to light mode" : "Switch to dark mode");
}

export function initTheme(toggle: HTMLElement | null = document.querySelector<HTMLElement>(".theme-toggle")): boolean {
  const system = window.matchMedia(DARK_SCHEME_QUERY);

  if (!isTheme(document.documentElement.dataset["theme"])) {
    applyTheme(storedTheme() ?? systemTheme(), false);
  } else {
    syncThemeColor(currentTheme());
  }

  if (toggle) updateToggleLabel(toggle, currentTheme());

  toggle?.addEventListener("click", () => {
    const next: Theme = currentTheme() === "dark" ? "light" : "dark";
    applyTheme(next, true);
    updateToggleLabel(toggle, next);
  });

  system.addEventListener("change", (event: MediaQueryListEvent) => {
    if (storedTheme() !== null) return;
    const next: Theme = event.matches ? "dark" : "light";
    applyTheme(next, false);
    if (toggle) updateToggleLabel(toggle, next);
  });

  window.addEventListener("storage", (event: StorageEvent) => {
    if (event.key !== THEME_STORAGE_KEY) return;
    const next = storedTheme() ?? systemTheme();
    applyTheme(next, false);
    if (toggle) updateToggleLabel(toggle, next);
  });

  return toggle !== null;
}
