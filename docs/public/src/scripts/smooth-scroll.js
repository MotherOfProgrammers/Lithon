/**
 * Smooth in-page scrolling for the `#anchor` links.
 *
 * `scroll-behavior: smooth` alone is not enough here: it cannot be cancelled by
 * the visitor, it ignores the sticky header's height, and it keeps easing even
 * after a new target is chosen. Driving it from script lets the scroll land
 * below the fixed header and lets a second click interrupt the first.
 *
 * Under `prefers-reduced-motion: reduce` this does nothing at all and the
 * browser's own instant jump — already offset by `scroll-padding-top` in the
 * base styles — takes over.
 */
const REDUCED_MOTION_QUERY = "(prefers-reduced-motion: reduce)";
/** Long enough to read as travel, short enough that it never feels like lag.
 *  Matches the deceleration curve used elsewhere in the system. */
const DURATION_MS = 520;
let activeScroll = 0;
function prefersReducedMotion() {
    return window.matchMedia(REDUCED_MOTION_QUERY).matches;
}
function easeOutQuint(t) {
    return 1 - Math.pow(1 - t, 5);
}
function stop() {
    if (activeScroll === 0)
        return;
    window.cancelAnimationFrame(activeScroll);
    activeScroll = 0;
}
function scrollToTarget(target) {
    const header = document.querySelector(".site-header");
    // The header is fixed, so the target needs its height added back or it lands
    // underneath it. scroll-padding-top covers the no-JS path; this is the JS one.
    const offset = (header?.offsetHeight ?? 0) + 24;
    const start = window.scrollY;
    const destination = target.getBoundingClientRect().top + start - offset;
    const distance = destination - start;
    if (Math.abs(distance) < 2) {
        // Already there: snap the hash so the URL still reflects the destination.
        if (window.location.hash !== `#${target.id}`) {
            window.history.replaceState(null, "", `#${target.id}`);
        }
        return;
    }
    const startedAt = performance.now();
    stop();
    const step = (now) => {
        const progress = Math.min((now - startedAt) / DURATION_MS, 1);
        window.scrollTo(0, start + distance * easeOutQuint(progress));
        if (progress < 1) {
            activeScroll = window.requestAnimationFrame(step);
        }
        else {
            activeScroll = 0;
        }
    };
    activeScroll = window.requestAnimationFrame(step);
    if (window.history.replaceState !== undefined && window.location.hash !== `#${target.id}`) {
        window.history.replaceState(null, "", `#${target.id}`);
    }
}
export function initSmoothScroll(scope = document) {
    document.addEventListener("click", (event) => {
        if (event.defaultPrevented || event.button !== 0)
            return;
        if (event.metaKey || event.ctrlKey || event.shiftKey || event.altKey)
            return;
        const anchor = event.target?.closest("a[href^='#']");
        if (anchor === null || anchor === undefined)
            return;
        const id = anchor.getAttribute("href")?.slice(1) ?? "";
        if (id === "")
            return;
        const target = document.getElementById(id);
        if (target === null)
            return;
        // Reduced motion keeps the browser's own behaviour, which the base
        // stylesheet already offsets for the fixed header.
        if (prefersReducedMotion())
            return;
        event.preventDefault();
        stop();
        scrollToTarget(target);
        // Keyboard users still need the focus to follow the jump.
        target.setAttribute("tabindex", "-1");
        target.focus({ preventScroll: true });
    }, { passive: false });
    // A new gesture should win over an in-flight scroll.
    for (const event of ["wheel", "touchstart", "keydown"]) {
        window.addEventListener(event, stop, { passive: true });
    }
}
