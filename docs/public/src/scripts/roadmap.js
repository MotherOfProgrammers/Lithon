import { qs, qsa } from "./dom.js";
import { initSite } from "./site.js";
// "done" and "active" are separate on purpose. Work that runs but has no
// verifying test behind it yet is not shipped, and collapsing the two would
// hide exactly the distinction this tracker exists to make.
const FILTERS = ["all", "done", "active", "next", "later"];
function isFilter(value) {
    return FILTERS.includes(value ?? "");
}
/**
 * Phases behave as a single-open accordion: the reader is usually asking "what
 * is happening now", and Phase I is the answer, so opening one closes the rest
 * instead of letting three run down the page at once.
 */
function initPhaseAccordion() {
    const toggles = qsa("[data-roadmap-group-toggle]");
    if (toggles.length === 0)
        return;
    const setOpen = (toggle, open) => {
        toggle.setAttribute("aria-expanded", String(open));
        const body = document.getElementById(toggle.getAttribute("aria-controls") ?? "");
        if (body !== null)
            body.hidden = !open;
    };
    for (const toggle of toggles) {
        toggle.addEventListener("click", () => {
            const open = toggle.getAttribute("aria-expanded") !== "true";
            for (const other of toggles)
                setOpen(other, other === toggle && open);
        });
    }
}
/**
 * Filters across every phase at once, and hides a phase whose milestones are all
 * filtered out rather than leaving an empty header behind.
 */
function initRoadmapFilter() {
    const buttons = qsa("[data-roadmap-filter]");
    const items = qsa("[data-roadmap-status]");
    const groups = qsa("[data-roadmap-group]");
    const liveRegion = qs("[data-roadmap-live]");
    if (buttons.length === 0 || items.length === 0)
        return;
    const apply = (filter) => {
        for (const button of buttons) {
            const selected = button.dataset["roadmapFilter"] === filter;
            button.classList.toggle("is-selected", selected);
            button.setAttribute("aria-pressed", String(selected));
        }
        let visible = 0;
        for (const item of items) {
            const show = filter === "all" || item.dataset["roadmapStatus"] === filter;
            item.classList.toggle("is-hidden", !show);
            if (show)
                visible += 1;
        }
        for (const group of groups) {
            const body = qs("[data-roadmap-group-body]", group);
            const anyVisible = qs("[data-roadmap-status]:not(.is-hidden)", group) !== null;
            group.classList.toggle("is-hidden", !anyVisible);
            // A collapsed phase must not stay collapsed-but-empty: opening it after a
            // filter would otherwise reveal a phase with nothing in it.
            if (body !== null && !body.hidden && !anyVisible)
                body.hidden = true;
        }
        if (liveRegion !== null) {
            liveRegion.textContent = `Showing ${visible} of ${items.length} milestones.`;
        }
    };
    for (const button of buttons) {
        button.addEventListener("click", () => {
            const filter = button.dataset["roadmapFilter"];
            if (isFilter(filter))
                apply(filter);
        });
    }
    const initial = buttons.find((button) => button.classList.contains("is-selected"))?.dataset["roadmapFilter"];
    apply(isFilter(initial) ? initial : "all");
}
/**
 * Milestone detail opens in a native <dialog>, so focus trapping, Escape, and
 * the inert background come from the platform rather than being rebuilt here.
 *
 * The detail markup is copied out of the row it belongs to, which means it stays
 * in the document: findable by in-page search, readable by a reader whose module
 * never loaded, and impossible to drift out of sync with the title beside it.
 */
function initMilestoneDialog() {
    const dialog = qs("[data-milestone-dialog]");
    const body = qs("[data-milestone-body]", dialog ?? undefined);
    const scope = qs("[data-milestone-scope]", dialog ?? undefined);
    const close = qs("[data-milestone-close]", dialog ?? undefined);
    const openers = qsa("[data-milestone-open]");
    if (dialog === null || body === null || scope === null || close === null)
        return;
    if (openers.length === 0 || typeof dialog.showModal !== "function")
        return;
    for (const opener of openers) {
        opener.addEventListener("click", () => {
            const item = opener.closest("[data-roadmap-status]");
            const detail = item === null ? null : qs(".milestone-detail", item);
            const state = item === null ? null : qs(".roadmap-state", item);
            const group = opener.closest("[data-roadmap-group]");
            if (detail === null || state === null || group === null)
                return;
            const status = item?.dataset["roadmapStatus"] ?? "";
            const heading = document.createElement("h3");
            heading.id = "milestone-dialog-title";
            heading.textContent = opener.textContent?.replace("Open details", "").trim() ?? "";
            const chip = document.createElement("span");
            chip.className = "roadmap-state";
            // The chip's colour is keyed off an ancestor's data-roadmap-status, so the
            // status has to travel with the copy or the clone renders as a plain outline.
            chip.dataset["roadmapStatus"] = status;
            chip.textContent = state.textContent?.trim() ?? "";
            const phase = qs(".roadmap-group-phase", group);
            const detailCopy = document.createDocumentFragment();
            for (const child of Array.from(detail.childNodes))
                detailCopy.append(child);
            body.replaceChildren(heading, chip, detailCopy);
            scope.textContent = `${phase?.textContent?.trim() ?? "Roadmap"} · milestone`;
            dialog.showModal();
        });
    }
    close.addEventListener("click", () => dialog.close());
    // A click that lands on the backdrop is a click on the dialog element itself,
    // because the panel is the only thing the dialog box contains.
    dialog.addEventListener("click", (event) => {
        if (event.target === dialog)
            dialog.close();
    });
}
initSite();
initPhaseAccordion();
initRoadmapFilter();
initMilestoneDialog();
