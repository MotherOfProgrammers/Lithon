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
    for (const link of links) {
        if (withoutFragment(link.href) !== current)
            continue;
        link.classList.add("is-active");
        link.setAttribute("aria-current", "page");
    }
}
