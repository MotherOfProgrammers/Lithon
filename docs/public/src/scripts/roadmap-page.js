import { initSideIndex } from "./side-index.js";
import { initSite } from "./site.js";
// Entry script for the roadmap deep dives. Identical behaviour to the docs
// page: shared chrome, plus the "On this page" scrollspy. The roadmap hub has
// its own script because it filters milestones instead of scrolling sections.
initSite();
initSideIndex();
