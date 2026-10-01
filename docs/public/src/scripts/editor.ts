/**
 * Tokeniser for Lithon's Python-flavoured source.
 *
 * Runs as a single pass with one combined pattern rather than a chain of
 * regexes, so a long line costs one scan instead of a dozen. Emits typed spans
 * that the overlay renders; the textarea itself stays the single source of
 * truth for the text, which is what keeps this dependency-free and keeps
 * native undo, IME, and selection intact.
 */

export type TokenKind =
  | "keyword"
  | "builtin"
  | "type"
  | "number"
  | "string"
  | "comment"
  | "decorator"
  | "operator"
  | "punctuation"
  | "name"
  | "plain";

export interface Token {
  readonly kind: TokenKind;
  readonly text: string;
}

const KEYWORDS = new Set([
  "and", "as", "assert", "async", "await", "break", "class", "continue", "def", "del", "elif",
  "else", "except", "False", "finally", "for", "from", "global", "if", "import", "in", "is",
  "lambda", "None", "nonlocal", "not", "or", "pass", "raise", "return", "True", "try", "while",
  "with", "yield",
]);

const BUILTINS = new Set([
  "abs", "all", "any", "bool", "dict", "enumerate", "filter", "float", "int", "len", "list",
  "map", "max", "min", "print", "range", "round", "set", "sorted", "str", "sum", "tuple", "zip",
]);

/** The type names the tokeniser colours. Note that `bool` never takes a width:
 *  the checker rejects `bool[N]`, so it is the one annotation written bare. */
const TYPES = new Set(["int", "float", "bool", "str"]);

/** What the autocomplete actually offers — the full annotations a binding can
 *  legally carry. Offering a bare `int` would insert something the checker
 *  refuses with "type 'int' requires an explicit size". */
const TYPE_ANNOTATIONS: readonly Completion[] = [
  { label: "int[8]", detail: "signed integer · -128..127", insert: "int[8]" },
  { label: "int[16]", detail: "signed integer · -32768..32767", insert: "int[16]" },
  { label: "int[32]", detail: "signed integer · 32-bit", insert: "int[32]" },
  { label: "int[64]", detail: "signed integer · bare literals default here", insert: "int[64]" },
  { label: "float[32]", detail: "single precision", insert: "float[32]" },
  { label: "float[64]", detail: "double precision", insert: "float[64]" },
  { label: "bool", detail: "no width parameter", insert: "bool" },
];

const OPERATOR_CHARS = "+-*/%<>=!&|^~";
const PUNCTUATION_CHARS = "()[]{},:;.";

function charClass(chars: string): string {
  // Escape the range operator itself, otherwise "+-*" reads as a span from + to *.
  return `[${chars.replace(/[\\\]^\-]/g, "\\$&")}]`;
}

// Longest alternatives first: `**=` must win over `*=`, and `//` over `/`.
const TOKEN_PATTERN = new RegExp(
  [
    "(?<comment>#[^\\n]*)",
    '(?<string>(?:[frb]{0,2})(?:"""[\\s\\S]*?"""|\'\'\'[\\s\\S]*?\'\'\'|"(?:\\\\.|[^"\\\\\\n])*"|\'(?:\\\\.|[^\'\\\\\\n])*\'))',
    "(?<decorator>@[A-Za-z_][A-Za-z0-9_]*)",
    "(?<arrow>->)",
    "(?<number>\\b(?:0[xXbBoO][0-9a-fA-F_]+|\\d[\\d_]*\\.?\\d[\\d_]*(?:[eE][+-]?\\d+)?|\\d[\\d_]*\\.)\\b)",
    "(?<name>[A-Za-z_][A-Za-z0-9_]*)",
    `(?<operator>\\*\\*=|//=|<<|>>|[*/%]=|\\*\\*|//|${charClass(OPERATOR_CHARS)})`,
    `(?<punctuation>${charClass(PUNCTUATION_CHARS)})`,
  ].join("|"),
  "g",
);

function classify(name: string): TokenKind {
  if (KEYWORDS.has(name)) return "keyword";
  if (TYPES.has(name)) return "type";
  if (BUILTINS.has(name)) return "builtin";
  return "name";
}

export function tokenize(source: string): Token[] {
  const tokens: Token[] = [];
  let cursor = 0;

  const push = (kind: TokenKind, text: string): void => {
    if (text === "") return;
    const last = tokens[tokens.length - 1];
    // Coalesce runs of the same kind so the DOM gets fewer, longer spans.
    if (last !== undefined && last.kind === kind) {
      tokens[tokens.length - 1] = { kind, text: last.text + text };
      return;
    }
    tokens.push({ kind, text });
  };

  TOKEN_PATTERN.lastIndex = 0;
  let match: RegExpExecArray | null = TOKEN_PATTERN.exec(source);

  while (match !== null) {
    const groups = match.groups ?? {};
    push("plain", source.slice(cursor, match.index));

    if (groups["comment"] !== undefined) push("comment", groups["comment"]);
    else if (groups["string"] !== undefined) push("string", groups["string"]);
    else if (groups["decorator"] !== undefined) push("decorator", groups["decorator"]);
    else if (groups["arrow"] !== undefined) push("operator", groups["arrow"]);
    else if (groups["number"] !== undefined) push("number", groups["number"]);
    else if (groups["name"] !== undefined) push(classify(groups["name"]), groups["name"]);
    else if (groups["operator"] !== undefined) push("operator", groups["operator"]);
    else if (groups["punctuation"] !== undefined) push("punctuation", groups["punctuation"]);

    cursor = match.index + match[0].length;
    match = TOKEN_PATTERN.exec(source);
  }

  push("plain", source.slice(cursor));
  return tokens;
}

const ESCAPES: Readonly<Record<string, string>> = {
  "&": "&amp;",
  "<": "&lt;",
  ">": "&gt;",
};

export function escapeHtml(value: string): string {
  return value.replace(/[&<>]/g, (character) => ESCAPES[character] ?? character);
}

const CLASS_PREFIX = "tok-";

/** Renders tokens to the overlay markup. Every token is wrapped, including
 *  `plain`, so the overlay and the textarea can never drift out of sync on
 *  whitespace. */
export function highlightToHtml(source: string): string {
  const tokens = tokenize(source);
  // A trailing newline needs a filler line or the overlay's last line collapses
  // and the two layers stop lining up.
  const trailing = source.endsWith("\n") ? "\n" : "";
  return (
    tokens
      .map(
        (token) =>
          `<span class="${CLASS_PREFIX}${token.kind}">${escapeHtml(token.text)}</span>`,
      )
      .join("") + trailing
  );
}

export interface Completion {
  readonly label: string;
  readonly detail: string;
  readonly insert: string;
}

/** Suggestions for the current token. `prefix` is what the visitor has typed
 *  so far, already trimmed of any leading dot. */
export function completionsFor(prefix: string, isMemberAccess: boolean): Completion[] {
  const query = prefix.toLowerCase();
  const matches = (items: readonly Completion[]): Completion[] =>
    items.filter((item) => item.label.toLowerCase().startsWith(query)).slice(0, 8);

  if (isMemberAccess) return matches(MEMBERS);

  const keywords: Completion[] = [...KEYWORDS]
    .filter((word) => /^[a-z]/.test(word))
    .map((word) => ({ label: word, detail: "keyword", insert: word }));
  const builtins: Completion[] = [...BUILTINS].map((word) => ({
    label: word,
    detail: "builtin",
    insert: word,
  }));
  return [...matches(TYPE_ANNOTATIONS), ...matches(builtins), ...matches(keywords)];
}

const MEMBERS: readonly Completion[] = [
  { label: "upper", detail: "str method", insert: "upper()" },
  { label: "lower", detail: "str method", insert: "lower()" },
  { label: "strip", detail: "str method", insert: "strip()" },
  { label: "split", detail: "str method", insert: "split()" },
  { label: "join", detail: "str method", insert: "join()" },
  { label: "append", detail: "list method", insert: "append(" },
  { label: "keys", detail: "dict method", insert: "keys()" },
  { label: "values", detail: "dict method", insert: "values()" },
  { label: "items", detail: "dict method", insert: "items()" },
];

/** Reads the token the caret currently sits in, plus whether it follows a dot. */
export interface CaretContext {
  readonly prefix: string;
  readonly start: number;
  readonly isMemberAccess: boolean;
}

export function caretContext(source: string, caret: number): CaretContext {
  let start = caret;
  while (start > 0) {
    const character = source[start - 1];
    if (character === undefined || !/[A-Za-z0-9_]/.test(character)) break;
    start -= 1;
  }
  const prefix = source.slice(start, caret);
  const before = source[start - 1];
  return { prefix, start, isMemberAccess: before === "." };
}
