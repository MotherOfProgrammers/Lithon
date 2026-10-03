import { qsa } from "./dom.js";
function withoutFragment(href) {
    const url = new URL(href, window.location.href);
    url.hash = "";
    url.search = "";
    return url.href;
}
export function markActiveNavLink(scope = document) {
    const links = qsa(".nav-links a", scope);
    if (links.length === 0)
        return;
    const current = withoutFragment(window.location.href);
    const brand = scope.querySelector(".nav .brand");
    const siteIndex = brand === null ? null : withoutFragment(brand.href);
    if (siteIndex !== null && current === siteIndex)
        return;
    // A page nested under a section — a roadmap deep dive, say — has no href of
    // its own in the nav, so an exact comparison alone would leave the reader with
    // no indication of where they are. A link may therefore claim a section by
    // name, and the page names the section it belongs to. The body attribute is
    // what makes this work for a link that is not the current page.
    const pageSection = document.body.dataset["navSection"] ?? null;
    for (const link of links) {
        const isCurrent = withoutFragment(link.href) === current ||
            (pageSection !== null && link.dataset["navSection"] === pageSection);
        if (!isCurrent)
            continue;
        link.classList.add("is-active");
        link.setAttribute("aria-current", "page");
    }
}
