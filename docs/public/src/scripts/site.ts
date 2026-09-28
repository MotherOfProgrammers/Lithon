import { initHeaderScroll, initMobileMenu } from "./chrome.js";
import { qs } from "./dom.js";
import { initControlFeedback } from "./feedback.js";
import { markActiveNavLink } from "./nav.js";
import { initReveal } from "./reveal.js";
import { initSmoothScroll } from "./smooth-scroll.js";
import { initTheme } from "./theme.js";

function claimPendingFlag(): void {
  delete document.documentElement.dataset["jsPending"];
}

export function initSite(): void {
  // Marks the document ready so the header and first screen can start their
  // entrance. Claiming the pending flag comes last: if anything above throws,
  // the head fallback still strips .js and the page falls back to being plain.
  document.documentElement.classList.add("is-ready");
  initTheme(qs(".theme-toggle"));
  initHeaderScroll(qs(".site-header"));
  initMobileMenu(qs(".menu-toggle"), qs(".nav-links"));
  markActiveNavLink();
  initReveal();
  initSmoothScroll();
  initControlFeedback(document, qs(".sound-toggle"));
  claimPendingFlag();
}
