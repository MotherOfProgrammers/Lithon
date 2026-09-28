/**
 * Wires syntax highlighting and autocomplete onto the existing textarea.
 *
 * The textarea is never replaced or hidden. A `<pre>` overlay sits behind it,
 * painted from the same text, and the textarea's own text colour is made
 * transparent so only the overlay's colours are seen. That keeps every native
 * behaviour the browser already gets right — undo, spellcheck off, IME
 * composition, selection, caret, mobile keyboards — while still colouring the
 * code.
 */
import { caretContext, completionsFor, highlightToHtml } from "./editor.js";
const INDENT = "  ";
const TOGGLE_KEY = "Tab";
const ACCEPT_KEYS = new Set(["Enter", "Tab"]);
const DISMISS_KEYS = new Set(["Escape"]);
export function initCodeEditor(parts) {
    const { textarea, highlight, suggestions } = parts;
    if (textarea === null || highlight === null || suggestions === null)
        return;
    let state = null;
    const paint = () => {
        highlight.innerHTML = highlightToHtml(textarea.value);
    };
    const closeSuggestions = () => {
        state = null;
        suggestions.hidden = true;
        suggestions.replaceChildren();
    };
    const openSuggestions = () => {
        const context = caretContext(textarea.value, textarea.selectionStart);
        const items = completionsFor(context.prefix, context.isMemberAccess);
        if (items.length === 0 || context.prefix.length === 0) {
            closeSuggestions();
            return;
        }
        state = { items, from: context.start, active: 0 };
        const tokenStart = context.start;
        const list = document.createElement("ul");
        list.className = "suggestion-list";
        list.setAttribute("role", "listbox");
        items.forEach((item, index) => {
            const row = document.createElement("li");
            row.className = "suggestion";
            row.id = `suggestion-${index}`;
            row.setAttribute("role", "option");
            row.setAttribute("aria-selected", String(index === 0));
            const label = document.createElement("span");
            label.className = "suggestion__label";
            label.textContent = item.label;
            const detail = document.createElement("span");
            detail.className = "suggestion__detail";
            detail.textContent = item.detail;
            row.append(label, detail);
            list.append(row);
        });
        suggestions.replaceChildren(list);
        suggestions.hidden = false;
        positionSuggestions(tokenStart);
    };
    /** Places the popup on the caret's line, in the textarea's own coordinates. */
    const positionSuggestions = (from) => {
        const upto = textarea.value.slice(0, from);
        const line = upto.split("\n").length - 1;
        const column = upto.length - (upto.lastIndexOf("\n") + 1);
        const style = window.getComputedStyle(textarea);
        const lineHeight = Number.parseFloat(style.lineHeight) || 20;
        const charWidth = measureCharWidth(textarea, style);
        const padTop = Number.parseFloat(style.paddingTop) || 0;
        const padLeft = Number.parseFloat(style.paddingLeft) || 0;
        suggestions.style.left = `${padLeft + column * charWidth}px`;
        suggestions.style.top = `${padTop + (line + 1) * lineHeight + 4}px`;
    };
    const setActive = (index) => {
        if (state === null)
            return;
        const count = state.items.length;
        state.active = ((index % count) + count) % count;
        suggestions.querySelectorAll(".suggestion").forEach((row, position) => {
            row.setAttribute("aria-selected", String(position === state?.active));
        });
    };
    const accept = () => {
        if (state === null)
            return false;
        const item = state.items[state.active];
        if (item === undefined)
            return false;
        const start = state.from;
        const end = textarea.selectionStart;
        textarea.setRangeText(item.insert, start, end, "end");
        paint();
        closeSuggestions();
        return true;
    };
    textarea.addEventListener("input", () => {
        paint();
        openSuggestions();
    });
    textarea.addEventListener("click", closeSuggestions);
    textarea.addEventListener("blur", () => window.setTimeout(closeSuggestions, 120));
    textarea.addEventListener("keydown", (event) => {
        if (state !== null) {
            if (event.key === "ArrowDown") {
                event.preventDefault();
                setActive(state.active + 1);
                return;
            }
            if (event.key === "ArrowUp") {
                event.preventDefault();
                setActive(state.active - 1);
                return;
            }
            if (ACCEPT_KEYS.has(event.key)) {
                event.preventDefault();
                accept();
                return;
            }
            if (DISMISS_KEYS.has(event.key)) {
                event.preventDefault();
                closeSuggestions();
                return;
            }
        }
        if (event.key === TOGGLE_KEY && !event.ctrlKey && !event.metaKey && !event.altKey) {
            event.preventDefault();
            const { selectionStart, selectionEnd, value } = textarea;
            const lineStart = value.lastIndexOf("\n", selectionStart - 1) + 1;
            if (selectionStart === selectionEnd) {
                const lineEnd = value.indexOf("\n", selectionEnd);
                const stop = lineEnd === -1 ? value.length : lineEnd;
                const line = value.slice(lineStart, stop);
                if (line.length > 0 && /^\s+$/.test(line)) {
                    // Shifting out: strip one indent level from the current line.
                    const dedented = line.startsWith(INDENT) ? line.slice(INDENT.length) : line.replace(/^\s/, "");
                    textarea.setRangeText(dedented, lineStart, stop, "end");
                    paint();
                }
                else {
                    textarea.setRangeText(INDENT, selectionStart, selectionEnd, "end");
                    paint();
                }
                return;
            }
            // A real selection: indent or dedent every line it touches.
            const blockEnd = value.indexOf("\n", selectionEnd);
            const lastLine = blockEnd === -1 ? value.length : blockEnd;
            const block = value.slice(lineStart, lastLine);
            const shifted = event.shiftKey
                ? block.replace(new RegExp(`^${INDENT}`, "gm"), "")
                : block.replace(/^/gm, INDENT);
            textarea.setRangeText(shifted, lineStart, lastLine, "preserve");
            textarea.setSelectionRange(lineStart, lineStart + shifted.length);
            paint();
            return;
        }
        if (event.key === "Enter") {
            // Keep the current indent, and open a block one level deeper after a colon.
            const { selectionStart, selectionEnd, value } = textarea;
            const lineStart = value.lastIndexOf("\n", selectionStart - 1) + 1;
            const currentLine = value.slice(lineStart, selectionStart);
            const indent = /^\s*/.exec(currentLine)?.[0] ?? "";
            const opensBlock = /:\s*$/.test(currentLine);
            const insert = `\n${indent}${opensBlock ? INDENT : ""}`;
            event.preventDefault();
            textarea.setRangeText(insert, selectionStart, selectionEnd, "end");
            paint();
            openSuggestions();
        }
    });
    // A window resize or font swap changes the caret metrics the popup is
    // positioned against, so re-place it rather than let it drift.
    window.addEventListener("resize", () => {
        if (state !== null)
            positionSuggestions(state.from);
    });
    // Only now, with the overlay painted and known to be tracking the textarea,
    // is it safe to hide the textarea's own glyphs.
    textarea.classList.add("is-highlighted");
    paint();
}
/** Average advance width of the editor's monospace face, cached per font. */
let charWidthCache = null;
function measureCharWidth(textarea, style) {
    const font = `${style.fontWeight} ${style.fontSize}/${style.lineHeight} ${style.fontFamily}`;
    if (charWidthCache !== null && charWidthCache.font === font)
        return charWidthCache.width;
    const probe = document.createElement("span");
    probe.style.position = "absolute";
    probe.style.visibility = "hidden";
    probe.style.whiteSpace = "pre";
    probe.style.font = style.font;
    probe.textContent = "0".repeat(50);
    textarea.parentElement?.append(probe);
    const width = probe.getBoundingClientRect().width / 50;
    probe.remove();
    // Guard against a zero or absurd measurement from a not-yet-loaded face.
    const safe = width > 0 ? width : Number.parseFloat(style.fontSize) * 0.6;
    charWidthCache = { font, width: safe };
    return safe;
}
