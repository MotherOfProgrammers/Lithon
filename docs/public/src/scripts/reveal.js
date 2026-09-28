import { qsa } from "./dom.js";
const REDUCED_MOTION_QUERY = "(prefers-reduced-motion: reduce)";
function showAll(targets) {
    for (const target of targets)
        target.classList.add("is-visible");
}
/**
 * Blocks start hidden only under .js (see base.css) and are released as they
 * enter the viewport, so the first screen settles in on load and the rest
 * arrive on scroll. Anything already on screen animates on the first callback,
 * which is what makes the page feel like it is booting rather than blinking.
 */
export function initReveal(scope = document) {
    const targets = qsa(".reveal", scope).filter((target) => !target.classList.contains("is-visible"));
    if (targets.length === 0)
        return;
    if (window.matchMedia(REDUCED_MOTION_QUERY).matches || typeof IntersectionObserver !== "function") {
        showAll(targets);
        return;
    }
    const observer = new IntersectionObserver((entries) => {
        for (const entry of entries) {
            if (!entry.isIntersecting)
                continue;
            entry.target.classList.add("is-visible");
            observer.unobserve(entry.target);
        }
    }, { rootMargin: "0px 0px -6% 0px", threshold: 0.05 });
    for (const target of targets)
        observer.observe(target);
}
