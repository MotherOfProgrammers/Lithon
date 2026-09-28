import { qs } from "./dom.js";
import { initSite } from "./site.js";

const QUICKSTART_COMMANDS: readonly string[] = [
  "git clone https://github.com/MotherOfProgrammers/Lithon.git",
  "cd Lithon",
  "make -j$(nproc)",
  "./lithon run examples/fib.py",
];

const FEEDBACK_MS = 1600;

function copyViaSelection(text: string): boolean {
  const area = document.createElement("textarea");
  area.value = text;
  area.setAttribute("readonly", "");
  area.style.position = "fixed";
  area.style.top = "0";
  area.style.left = "0";
  area.style.opacity = "0";
  document.body.append(area);
  area.select();

  let copied = false;
  try {
    copied = document.execCommand("copy");
  } catch {
    copied = false;
  }

  area.remove();
  return copied;
}

async function copyText(text: string): Promise<boolean> {
  if (typeof navigator.clipboard?.writeText === "function") {
    try {
      await navigator.clipboard.writeText(text);
      return true;
    } catch {}
  }
  return copyViaSelection(text);
}

function initCopyButton(): void {
  const button = qs<HTMLButtonElement>(".copy-button");
  if (button === null) return;

  const idleLabel = (button.textContent ?? "copy").trim();
  let resetTimer = 0;

  button.addEventListener("click", () => {
    void copyText(QUICKSTART_COMMANDS.join("\n")).then((copied) => {
      window.clearTimeout(resetTimer);
      button.textContent = copied ? "copied" : "select & copy";
      button.setAttribute("aria-label", copied ? "Quickstart commands copied" : "Copying failed, select the commands manually");

      resetTimer = window.setTimeout(() => {
        button.textContent = idleLabel;
        button.setAttribute("aria-label", "Copy the quickstart commands");
      }, FEEDBACK_MS);
    });
  });
}

initSite();
initCopyButton();
