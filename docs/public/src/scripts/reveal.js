import { qsa } from "./dom.js";
function showAll(targets) {
    for (const target of targets)
        target.classList.add("is-visible");
}
export function initReveal(scope = document) {
    const targets = qsa(".reveal", scope);
    if (targets.length === 0)
        return;
    showAll(targets);
}
