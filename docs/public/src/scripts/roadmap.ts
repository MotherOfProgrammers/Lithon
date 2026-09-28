import { qs, qsa } from "./dom.js";
import { initSite } from "./site.js";

const FILTERS = ["all", "done", "next"] as const;
type Filter = (typeof FILTERS)[number];

function isFilter(value: string | undefined): value is Filter {
  return (FILTERS as readonly string[]).includes(value ?? "");
}

function initRoadmapFilter(): void {
  const buttons = qsa<HTMLButtonElement>("[data-roadmap-filter]");
  const items = qsa<HTMLElement>("[data-roadmap-status]");
  const liveRegion = qs("[data-roadmap-live]");

  if (buttons.length === 0 || items.length === 0) return;

  const apply = (filter: Filter): void => {
    for (const button of buttons) {
      const selected = button.dataset["roadmapFilter"] === filter;
      button.classList.toggle("is-selected", selected);
      button.setAttribute("aria-pressed", String(selected));
    }

    let visible = 0;
    for (const item of items) {
      const show = filter === "all" || item.dataset["roadmapStatus"] === filter;
      item.classList.toggle("is-hidden", !show);
      if (show) visible += 1;
    }

    if (liveRegion !== null) {
      liveRegion.textContent = `Showing ${visible} of ${items.length} milestones.`;
    }
  };

  for (const button of buttons) {
    button.addEventListener("click", () => {
      const filter = button.dataset["roadmapFilter"];
      if (isFilter(filter)) apply(filter);
    });
  }

  const initial = buttons.find((button) => button.classList.contains("is-selected"))?.dataset["roadmapFilter"];
  apply(isFilter(initial) ? initial : "all");
}

initSite();
initRoadmapFilter();
