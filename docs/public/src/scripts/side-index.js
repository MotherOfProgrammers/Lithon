import { onNextFrame, qs, qsa } from "./dom.js";
const ACTIVE_OFFSET_PX = 32;
function headerOffset() {
    const header = qs(".site-header");
    return (header?.offsetHeight ?? 86) + ACTIVE_OFFSET_PX;
}
/**
 * Marks the "On this page" entry whose target is the highest one scrolled past.
 *
 * Lives apart from any single page's entry script because both the documentation
 * page and every roadmap deep dive need it, and two copies of a scrollspy would
 * drift apart the first time the offset changed.
 */
export function initSideIndex() {
    const index = qs(".side-index");
    if (index === null)
        return;
    const column = qs(".content-column");
    if (column === null)
        return;
    const links = new Map();
    for (const link of qsa('a[href^="#"]', index)) {
        const id = link.getAttribute("href")?.slice(1) ?? "";
        if (id !== "" && column.querySelector(`#${CSS.escape(id)}`) !== null)
            links.set(id, link);
    }
    if (links.size === 0)
        return;
    let activeId = null;
    const update = () => {
        const offset = headerOffset();
        let current = null;
        for (const id of links.keys()) {
            const block = column.querySelector(`#${CSS.escape(id)}`);
            if (block !== null && block.getBoundingClientRect().top <= offset)
                current = id;
        }
        if (current === null)
            current = links.keys().next().value ?? null;
        if (current === activeId)
            return;
        activeId = current;
        for (const [id, link] of links) {
            const isCurrent = id === current;
            link.classList.toggle("is-current", isCurrent);
            if (isCurrent)
                link.setAttribute("aria-current", "true");
            else
                link.removeAttribute("aria-current");
        }
    };
    update();
    window.addEventListener("scroll", onNextFrame(update), { passive: true });
    window.addEventListener("resize", onNextFrame(update));
    window.addEventListener("hashchange", update);
}
