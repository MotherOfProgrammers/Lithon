import { initHeaderScroll, initMobileMenu } from "./chrome.js";
import { qs } from "./dom.js";
import { markActiveNavLink } from "./nav.js";
import { initReveal } from "./reveal.js";
import { initTheme } from "./theme.js";

function claimPendingFlag(): void {
  delete document.documentElement.dataset["jsPending"];
}

export function initSite(): void {
  claimPendingFlag();
  initTheme(qs(".theme-toggle"));
  initHeaderScroll(qs(".site-header"));
  initMobileMenu(qs(".menu-toggle"), qs(".nav-links"));
  markActiveNavLink();
  initReveal();
}
