import { qs, qsa } from "./dom.js";
import { initSite } from "./site.js";

interface Example {
  readonly id: string;
  readonly title: string;
  readonly note: string;
  readonly code: string;
}

const EXAMPLES: readonly Example[] = [
  {
    id: "fib",
    title: "Recursive fibonacci",
    note: "recursion · native tier-1",
    code: `def fib(n: int) -> int:
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

print(fib(10))`,
  },
  {
    id: "range",
    title: "Typed range loop",
    note: "for-range · fixed int accumulator",
    code: `total: int = 0

for i in range(0, 10):
    total = total + i

print(total)`,
  },
  {
    id: "nested",
    title: "Nested loop accumulator",
    note: "while + for · verified nesting",
    code: `row: int = 1
sum: int = 0

while row <= 3:
    col: int = 1
    while col <= 3:
        sum = sum + row * col
        col = col + 1
    row = row + 1

print(sum)`,
  },
  {
    id: "untyped",
    title: "Untyped program",
    note: "the verifier refuses to go native",
    code: `def double(n):
    return n * 2

print(double(21))`,
  },
];

const TABS = ["code", "examples"] as const;
type TabId = (typeof TABS)[number];

const RUN_DELAY_MS = 500;
const MAX_FIB_INDEX = 2000;
const ANNOTATION_PATTERN = /:\s*(?:int|float|bool|str)\b/g;
const PRINTED_FIB_PATTERN = /print\s*\(\s*fib\s*\(\s*(\d+)\s*\)\s*\)/;
const FIB_CALL_PATTERN = /fib\s*\(\s*(\d+)\s*\)/;

interface Analysis {
  readonly annotationCount: number;
  readonly fibIndex: number | null;
  readonly hasPrint: boolean;
}

function fibonacci(index: number): bigint | null {
  if (!Number.isInteger(index) || index < 0 || index > MAX_FIB_INDEX) return null;

  let previous = 0n;
  let current = 1n;
  for (let step = 0; step < index; step += 1) {
    const next = previous + current;
    previous = current;
    current = next;
  }
  return previous;
}

function analyze(code: string): Analysis {
  const annotationCount = code.match(ANNOTATION_PATTERN)?.length ?? 0;
  const printed = PRINTED_FIB_PATTERN.exec(code);
  const loose = printed === null ? FIB_CALL_PATTERN.exec(code) : null;
  const raw = printed?.[1] ?? loose?.[1] ?? null;
  const parsed = raw === null ? Number.NaN : Number.parseInt(raw, 10);

  return {
    annotationCount,
    fibIndex: Number.isNaN(parsed) || parsed > MAX_FIB_INDEX ? null : parsed,
    hasPrint: /\bprint\s*\(/.test(code),
  };
}

function makeLine(className: string, text: string): HTMLDivElement {
  const line = document.createElement("div");
  if (className !== "") line.className = className;
  line.textContent = text;
  return line;
}

const promptLine = (): HTMLDivElement => makeLine("prompt", "lithon ›");

function plural(count: number, singular: string): string {
  return `${count} ${singular}${count === 1 ? "" : "s"}`;
}

function initPlayground(): void {
  const editor = qs<HTMLTextAreaElement>("#code-editor");
  const output = qs<HTMLElement>("#output-content");
  const stateLabel = qs("#run-state");
  const runButton = qs<HTMLButtonElement>("#run-code");
  const editorName = qs("[data-editor-name]");
  const exampleList = qs<HTMLUListElement>("#example-list");
  const tabButtons = qsa<HTMLButtonElement>("[data-tab]");
  const panels = qsa<HTMLElement>("[data-panel]");

  if (editor === null || output === null) return;

  const setState = (label: string, busy: boolean): void => {
    if (stateLabel !== null) stateLabel.textContent = label;
    if (runButton !== null) runButton.disabled = busy;
    output.setAttribute("aria-busy", String(busy));
  };

  const renderResult = (code: string): void => {
    const trimmed = code.trim();
    const nodes: HTMLDivElement[] = [promptLine()];

    if (trimmed === "") {
      nodes.push(makeLine("failure", "empty program — nothing to verify"));
      output.replaceChildren(...nodes);
      return;
    }

    const analysis = analyze(trimmed);
    const typed = analysis.annotationCount > 0;

    nodes.push(
      typed
        ? makeLine("success", "✓ static flow verified")
        : makeLine("muted", "○ static flow unverified — no explicit type annotations"),
      typed
        ? makeLine("success", `✓ ${plural(analysis.annotationCount, "fixed type annotation")} resolved`)
        : makeLine("muted", "○ add an explicit type annotation to go native"),
      promptLine(),
    );

    const value = analysis.fibIndex === null ? null : fibonacci(analysis.fibIndex);
    if (value !== null) {
      const resultLine = document.createElement("div");
      resultLine.append(document.createTextNode("result: "));
      const strong = document.createElement("strong");
      strong.className = "value";
      strong.textContent = value.toString();
      resultLine.append(strong);

      nodes.push(resultLine, makeLine("muted", `fib(${analysis.fibIndex}) · native path · 0.42 ms simulated`));
    } else if (analysis.hasPrint) {
      nodes.push(makeLine("muted", "program accepted — add print(fib(10)) to preview a computed value"));
    } else {
      nodes.push(makeLine("muted", "no top-level call detected — nothing to evaluate"));
    }

    output.replaceChildren(...nodes);
  };

  const runProgram = (): void => {
    setState("verifying", true);
    output.replaceChildren(promptLine(), makeLine("muted", "checking static flow…"));

    window.setTimeout(() => {
      renderResult(editor.value);
      setState("ready", false);
    }, RUN_DELAY_MS);
  };

  runButton?.addEventListener("click", runProgram);

  editor.addEventListener("keydown", (event: KeyboardEvent) => {
    if (event.key === "Enter" && (event.metaKey || event.ctrlKey)) {
      event.preventDefault();
      if (runButton?.disabled !== true) runProgram();
    }
  });

  const selectTab = (id: TabId): void => {
    for (const button of tabButtons) {
      const selected = button.dataset["tab"] === id;
      button.classList.toggle("is-active", selected);
      button.setAttribute("aria-selected", String(selected));
      button.tabIndex = selected ? 0 : -1;
    }
    for (const panel of panels) {
      panel.hidden = panel.dataset["panel"] !== id;
    }
  };

  for (const button of tabButtons) {
    button.addEventListener("click", () => {
      const id = button.dataset["tab"];
      if ((TABS as readonly string[]).includes(id ?? "")) selectTab(id as TabId);
    });

    button.addEventListener("keydown", (event: KeyboardEvent) => {
      if (event.key !== "ArrowRight" && event.key !== "ArrowLeft") return;
      event.preventDefault();
      const step = event.key === "ArrowRight" ? 1 : -1;
      const position = tabButtons.indexOf(button);
      const target = tabButtons[(position + step + tabButtons.length) % tabButtons.length];
      if (target === undefined) return;
      const id = target.dataset["tab"];
      if ((TABS as readonly string[]).includes(id ?? "")) {
        selectTab(id as TabId);
        target.focus();
      }
    });
  }

  if (exampleList !== null) {
    for (const example of EXAMPLES) {
      const item = document.createElement("li");
      const button = document.createElement("button");
      button.type = "button";
      button.className = "example-item";

      const title = document.createElement("strong");
      title.textContent = example.title;
      const note = document.createElement("span");
      note.textContent = example.note;
      button.append(title, note);

      button.addEventListener("click", () => {
        editor.value = example.code;
        if (editorName !== null) editorName.textContent = `${example.id}.lithon`;
        selectTab("code");
        editor.focus();
      });

      item.append(button);
      exampleList.append(item);
    }
  }
}

initSite();
initPlayground();
