export const THEME_STORAGE_KEY = "lithon-theme";
const DARK_SCHEME_QUERY = "(prefers-color-scheme: dark)";
const THEME_COLORS = {
    light: "#f8fff4",
    dark: "#474350",
};
function isTheme(value) {
    return value === "light" || value === "dark";
}
function readStorage(key) {
    try {
        return window.localStorage.getItem(key);
    }
    catch {
        return null;
    }
}
function writeStorage(key, value) {
    try {
        window.localStorage.setItem(key, value);
    }
    catch { }
}
export function systemTheme() {
    return window.matchMedia(DARK_SCHEME_QUERY).matches ? "dark" : "light";
}
export function storedTheme() {
    const stored = readStorage(THEME_STORAGE_KEY);
    return isTheme(stored) ? stored : null;
}
export function currentTheme() {
    const applied = document.documentElement.dataset["theme"];
    return isTheme(applied) ? applied : systemTheme();
}
function syncThemeColor(theme) {
    const meta = document.querySelector('meta[name="theme-color"]');
    if (meta)
        meta.content = THEME_COLORS[theme];
}
export function applyTheme(theme, persist) {
    document.documentElement.dataset["theme"] = theme;
    if (persist)
        writeStorage(THEME_STORAGE_KEY, theme);
    syncThemeColor(theme);
}
function updateToggleLabel(toggle, theme) {
    toggle.setAttribute("aria-label", theme === "dark" ? "Switch to light mode" : "Switch to dark mode");
}
export function initTheme(toggle = document.querySelector(".theme-toggle")) {
    const system = window.matchMedia(DARK_SCHEME_QUERY);
    if (!isTheme(document.documentElement.dataset["theme"])) {
        applyTheme(storedTheme() ?? systemTheme(), false);
    }
    else {
        syncThemeColor(currentTheme());
    }
    if (toggle)
        updateToggleLabel(toggle, currentTheme());
    toggle?.addEventListener("click", () => {
        const next = currentTheme() === "dark" ? "light" : "dark";
        applyTheme(next, true);
        updateToggleLabel(toggle, next);
    });
    system.addEventListener("change", (event) => {
        if (storedTheme() !== null)
            return;
        const next = event.matches ? "dark" : "light";
        applyTheme(next, false);
        if (toggle)
            updateToggleLabel(toggle, next);
    });
    window.addEventListener("storage", (event) => {
        if (event.key !== THEME_STORAGE_KEY)
            return;
        const next = storedTheme() ?? systemTheme();
        applyTheme(next, false);
        if (toggle)
            updateToggleLabel(toggle, next);
    });
    return toggle !== null;
}
