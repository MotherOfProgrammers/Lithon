import { onNextFrame, qs } from "./dom.js";

const SCROLLED_AFTER_PX = 16;
const COMPACT_QUERY = "(max-width: 700px)";

export function initHeaderScroll(header: HTMLElement | null = qs(".site-header")): void {
  if (!header) return;

  let applied = header.classList.contains("is-scrolled");

  const update = (): void => {
    const shouldScroll = window.scrollY > SCROLLED_AFTER_PX;
    if (shouldScroll === applied) return;
    applied = shouldScroll;
    header.classList.toggle("is-scrolled", shouldScroll);
  };

  update();
  window.addEventListener("scroll", onNextFrame(update), { passive: true });
}

export interface MobileMenu {
  close: () => void;
}

export function initMobileMenu(
  toggle: HTMLElement | null = qs(".menu-toggle"),
  panel: HTMLElement | null = qs(".nav-links"),
): MobileMenu | null {
  if (!toggle || !panel) return null;

  const compact = window.matchMedia(COMPACT_QUERY);

  const setOpen = (open: boolean): void => {
    panel.classList.toggle("is-open", open);
    toggle.setAttribute("aria-expanded", String(open));
    toggle.setAttribute("aria-label", open ? "Close navigation" : "Open navigation");
  };

  const close = (): void => {
    if (panel.classList.contains("is-open")) setOpen(false);
  };

  toggle.addEventListener("click", () => {
    setOpen(!panel.classList.contains("is-open"));
  });

  panel.querySelectorAll("a").forEach((link) => {
    link.addEventListener("click", close);
  });

  document.addEventListener("keydown", (event: KeyboardEvent) => {
    if (event.key === "Escape" && panel.classList.contains("is-open")) {
      close();
      toggle.focus();
    }
  });

  document.addEventListener("pointerdown", (event: PointerEvent) => {
    if (!panel.classList.contains("is-open")) return;
    const target = event.target;
    if (target instanceof Node && (panel.contains(target) || toggle.contains(target))) return;
    close();
  });

  compact.addEventListener("change", (event: MediaQueryListEvent) => {
    if (!event.matches) close();
  });

  return { close };
}
