import { qsa } from "./dom.js";

function showAll(targets: readonly HTMLElement[]): void {
  for (const target of targets) target.classList.add("is-visible");
}

export function initReveal(scope: ParentNode = document): void {
  const targets = qsa<HTMLElement>(".reveal", scope);
  if (targets.length === 0) return;
  showAll(targets);
}
