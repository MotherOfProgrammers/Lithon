import { qs } from "./dom.js";
import { initSite } from "./site.js";

const QUICKSTART_COMMANDS: readonly string[] = [
  "git clone https://github.com/MotherOfProgrammers/Lithon.git",
  "cd Lithon",
  "cmake -B build -DCMAKE_BUILD_TYPE=Release",
  "cmake --build build -j$(nproc)",
  "python3 src/frontend/frontend.py tests/programs/float.py > /tmp/float.ir",
  "./build/tier_runner /tmp/float.ir --strict",
];

const FEEDBACK_MS = 1600;

const REPO = "MotherOfProgrammers/Lithon";
const CONTRIBUTORS_API = `https://api.github.com/repos/${REPO}/contributors?per_page=30`;
const CONTRIBUTORS_GRAPH = `https://github.com/${REPO}/graphs/contributors`;
const CONTRIBUTOR_LIMIT = 8;
const CONTRIBUTOR_SKELETONS = 8;
const REQUEST_TIMEOUT_MS = 8000;
/* The unauthenticated GitHub API allows 60 requests an hour per IP, which a
   page refresh loop burns through quickly. localStorage rather than
   sessionStorage, so one successful fetch covers later visits too — which is
   what makes the section still work on a later reload after the budget is
   spent. */
const CACHE_KEY = "lithon-contributors";
const CACHE_TTL_MS = 6 * 60 * 60 * 1000;

interface Contributor {
  readonly login: string;
  readonly avatarUrl: string;
  readonly profileUrl: string;
  readonly contributions: number;
}

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

class ContributorsRequestError extends Error {
  readonly status: number;

  constructor(status: number) {
    super(`GitHub answered with ${status}`);
    this.name = "ContributorsRequestError";
    this.status = status;
  }
}

function readContributorCache(): { contributors: Contributor[]; fetchedAt: number } | null {
  let raw: string | null;
  try {
    raw = window.localStorage.getItem(CACHE_KEY);
  } catch {
    return null;
  }
  if (raw === null) return null;

  try {
    const parsed: unknown = JSON.parse(raw);
    if (typeof parsed !== "object" || parsed === null) return null;
    const record = parsed as { contributors?: unknown; fetchedAt?: unknown };
    if (!Array.isArray(record.contributors) || typeof record.fetchedAt !== "number") return null;
    if (Date.now() - record.fetchedAt > CACHE_TTL_MS) return null;
    return {
      contributors: record.contributors.filter(isContributor),
      fetchedAt: record.fetchedAt,
    };
  } catch {
    return null;
  }
}

function writeContributorCache(contributors: Contributor[], fetchedAt: number): void {
  try {
    window.localStorage.setItem(CACHE_KEY, JSON.stringify({ contributors, fetchedAt }));
  } catch {
    /* A private-mode store is not worth failing the section over. */
  }
}

function isContributor(value: unknown): value is Contributor {
  if (typeof value !== "object" || value === null) return false;
  const record = value as Partial<Contributor>;
  return (
    typeof record.login === "string" &&
    typeof record.avatarUrl === "string" &&
    typeof record.profileUrl === "string" &&
    typeof record.contributions === "number"
  );
}

/** Keeps only accounts a visitor would recognise as people: bots and anonymous
 *  commits have no profile worth linking to. */
function toContributors(payload: unknown): Contributor[] {
  if (!Array.isArray(payload)) return [];

  const contributors: Contributor[] = [];
  for (const entry of payload) {
    if (typeof entry !== "object" || entry === null) continue;
    const record = entry as Record<string, unknown>;
    if (record.type === "Bot") continue;

    const login = typeof record.login === "string" ? record.login.trim() : "";
    if (login === "") continue;

    const contributions = typeof record.contributions === "number" ? record.contributions : 0;
    contributors.push({
      login,
      avatarUrl: typeof record.avatar_url === "string" ? record.avatar_url : "",
      profileUrl: typeof record.html_url === "string" ? record.html_url : `https://github.com/${login}`,
      contributions: Number.isFinite(contributions) ? contributions : 0,
    });
  }

  contributors.sort((a, b) => b.contributions - a.contributions);
  return contributors;
}

async function fetchContributors(signal: AbortSignal): Promise<Contributor[]> {
  const response = await fetch(CONTRIBUTORS_API, {
    headers: { Accept: "application/vnd.github+json" },
    signal,
  });
  if (!response.ok) throw new ContributorsRequestError(response.status);
  return toContributors(await response.json());
}

function initials(login: string): string {
  const words = login.split(/[^a-zA-Z0-9]+/).filter((word) => word !== "");
  const letters = words.slice(0, 2).map((word) => (word[0] ?? "").toUpperCase());
  return letters.join("") || login.slice(0, 2).toUpperCase();
}

function createProfileLink(href: string, label: string): HTMLAnchorElement {
  const link = document.createElement("a");
  link.className = "contributor-link";
  link.href = href;
  link.target = "_blank";
  link.rel = "noreferrer";
  link.setAttribute("aria-label", label);
  return link;
}

function createContributorItem(contributor: Contributor): HTMLLIElement {
  const item = document.createElement("li");
  item.className = "contributor";

  const commits = `${contributor.contributions} ${contributor.contributions === 1 ? "commit" : "commits"}`;
  const link = createProfileLink(contributor.profileUrl, `${contributor.login} on GitHub, ${commits}`);

  const avatar = document.createElement("span");
  avatar.className = "contributor-avatar";
  const fallback = document.createElement("span");
  fallback.className = "contributor-initials";
  fallback.textContent = initials(contributor.login);
  avatar.append(fallback);

  if (contributor.avatarUrl !== "") {
    const image = document.createElement("img");
    image.alt = "";
    image.loading = "lazy";
    image.decoding = "async";
    image.referrerPolicy = "no-referrer";
    image.src = contributor.avatarUrl;
    image.addEventListener("error", () => image.remove(), { once: true });
    avatar.append(image);
  }

  const login = document.createElement("span");
  login.className = "contributor-login";
  login.textContent = contributor.login;

  const meta = document.createElement("span");
  meta.className = "contributor-meta";
  meta.textContent = commits;

  link.append(avatar, login, meta);
  item.append(link);
  return item;
}

function createOverflowItem(remaining: number): HTMLLIElement {
  const item = document.createElement("li");
  item.className = "contributor contributor--more";
  const link = createProfileLink(CONTRIBUTORS_GRAPH, `${remaining} more contributors on GitHub`);
  link.textContent = `+${remaining} more`;
  item.append(link);
  return item;
}

function createSkeletonItems(): HTMLLIElement[] {
  const items: HTMLLIElement[] = [];
  for (let index = 0; index < CONTRIBUTOR_SKELETONS; index += 1) {
    const item = document.createElement("li");
    item.className = "contributor-slot";
    item.setAttribute("aria-hidden", "true");
    item.append(document.createElement("i"), document.createElement("i"));
    items.push(item);
  }
  return items;
}

function syncLabel(fetchedAt: number): string {
  const minutes = Math.max(0, Math.round((Date.now() - fetchedAt) / 60000));
  if (minutes < 1) return "just now";
  if (minutes === 1) return "a minute ago";
  if (minutes < 60) return `${minutes} minutes ago`;
  const hours = Math.round(minutes / 60);
  return hours === 1 ? "an hour ago" : `${hours} hours ago`;
}

function initContributors(): void {
  const found = qs<HTMLUListElement>("[data-contributors-list]");
  if (found === null) return;
  const list: HTMLUListElement = found;
  const status = qs<HTMLParagraphElement>("[data-contributors-status]");
  const counter = qs<HTMLSpanElement>("[data-contributors-count]");

  function showStatus(message: string): void {
    if (status === null) return;
    status.textContent = message;
  }

  /** A failed fetch leaves the section empty, so the one thing it can still do
   *  is offer another go. Without this the visitor is stuck until a reload. */
  function showRetry(message: string): void {
    if (status === null) {
      showStatus(message);
      return;
    }
    const button = document.createElement("button");
    button.type = "button";
    button.className = "contributors-retry";
    button.textContent = "retry";
    button.addEventListener("click", () => void load());

    status.replaceChildren(document.createTextNode(`${message} `), button);
  }

  function render(contributors: Contributor[], fetchedAt: number): void {
    const shown = contributors.slice(0, CONTRIBUTOR_LIMIT);
    const remaining = contributors.length - shown.length;
    const items = shown.map(createContributorItem);
    if (remaining > 0) items.push(createOverflowItem(remaining));

    list.replaceChildren(...items);
    list.removeAttribute("aria-busy");

    if (counter !== null) {
      counter.textContent = `${contributors.length} ${contributors.length === 1 ? "contributor" : "contributors"}`;
      counter.hidden = false;
    }
    showStatus(`Live from GitHub · synced ${syncLabel(fetchedAt)}`);
  }

  function clearList(): void {
    list.replaceChildren();
    list.removeAttribute("aria-busy");
    if (counter !== null) counter.hidden = true;
  }

  function describeFailure(error: unknown): string {
    if (error instanceof ContributorsRequestError) {
      return error.status === 403 || error.status === 429
        ? "GitHub's hourly request limit is spent for this network. The full contributor graph is still open above."
        : `GitHub answered ${error.status}. The full contributor graph is still open above.`;
    }
    return "Could not reach GitHub from here. The full contributor graph is still open above.";
  }

  async function load(): Promise<void> {
    const cached = readContributorCache();
    if (cached !== null && cached.contributors.length > 0) {
      render(cached.contributors, cached.fetchedAt);
      return;
    }

    clearList();
    list.setAttribute("aria-busy", "true");
    list.replaceChildren(...createSkeletonItems());
    showStatus("Reading the commit history from GitHub…");

    const controller = new AbortController();
    const timeoutId = window.setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);

    try {
      const contributors = await fetchContributors(controller.signal);
      if (contributors.length === 0) {
        clearList();
        showRetry("GitHub reported no contributors for this repository yet.");
        return;
      }
      const fetchedAt = Date.now();
      writeContributorCache(contributors, fetchedAt);
      render(contributors, fetchedAt);
    } catch (error: unknown) {
      clearList();
      showRetry(describeFailure(error));
    } finally {
      window.clearTimeout(timeoutId);
    }
  }

  void load();
}

initSite();
initCopyButton();
initContributors();
