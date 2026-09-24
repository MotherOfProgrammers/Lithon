(() => {
  const ICONS = {
    clock: '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 3"/>',
    sparkle:
      '<path d="M12 3l1.8 4.6L18.5 9l-4.7 1.4L12 15l-1.8-4.6L5.5 9l4.7-1.4L12 3z"/><path d="M18 15l.9 2.3L21 18l-2.1.7L18 21l-.9-2.3L15 18l2.1-.7L18 15z"/>',
    map: '<path d="M4 6h10l2 2h4v10H4z"/><path d="M4 6v12"/>',
    github:
      '<path d="M9 19c-4 1.5-4-2.5-6-3m12 5v-3.5c0-1 .1-1.4-.5-2 2.8-.3 5.5-1.4 5.5-6a4.6 4.6 0 0 0-1.3-3.2 4.3 4.3 0 0 0-.1-3.2s-1.1-.3-3.5 1.3a12 12 0 0 0-6 0C6.6 2.8 5.5 3.1 5.5 3.1a4.3 4.3 0 0 0-.1 3.2A4.6 4.6 0 0 0 4 9.5c0 4.6 2.7 5.7 5.5 6-.6.6-.6 1.2-.5 2V21"/>',
    grid:
      '<rect x="3" y="3" width="7" height="7" rx="1"/><rect x="14" y="3" width="7" height="7" rx="1"/><rect x="3" y="14" width="7" height="7" rx="1"/><path d="M17.5 14v7M14 17.5h7"/>',
    bolt: '<path d="M13 2L4 14h7l-1 8 9-12h-7l1-8z"/>',
    shield:
      '<path d="M12 3l7 3v5c0 4.5-3 8.5-7 10-4-1.5-7-5.5-7-10V6l7-3z"/><path d="M9 12l2 2 4-4"/>',
    book: '<path d="M4 5a2 2 0 0 1 2-2h12v18H6a2 2 0 0 1-2-2V5z"/><path d="M8 7h6M8 11h6"/>',
    rocket:
      '<path d="M4.5 16.5c-1.5 1.3-2 5-2 5s3.7-.5 5-2c.7-.8.7-2.1-.1-2.9-.8-.8-2.1-.7-2.9.1z"/><path d="M12 15l-3-3c.5-3.5 2.5-7.5 7-9.5 2-1 4.5-1.5 4.5-1.5s-.5 2.5-1.5 4.5c-2 4.5-6 6.5-9.5 7z"/><path d="M9 12H4s.5-3 2-4.5S11 6 11 6"/>',
    gauge:
      '<path d="M12 14l4-4"/><path d="M4.9 19a9 9 0 1 1 14.2 0"/>',
    terminal:
      '<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M7 9l3 3-3 3M13 15h4"/>',
    flask:
      '<path d="M9 3h6M10 3v6L5 19a2 2 0 0 0 1.8 3h10.4A2 2 0 0 0 19 19l-5-10V3"/><path d="M7.5 15h9"/>',
    layers:
      '<path d="M12 3l9 5-9 5-9-5 9-5z"/><path d="M3 13l9 5 9-5"/>',
    tools:
      '<path d="M14.7 6.3a4 4 0 0 0-5.4 5.4L3 18v3h3l6.3-6.3a4 4 0 0 0 5.4-5.4L15 12l-3-3 2.7-2.7z"/>',
    sun: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.93 4.93l1.41 1.41M17.66 17.66l1.41 1.41M2 12h2M20 12h2M4.93 19.07l1.41-1.41M17.66 6.34l1.41-1.41"/>',
    moon: '<path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"/>',
    search: '<circle cx="11" cy="11" r="7"/><path d="M21 21l-4.3-4.3"/>',
    quote:
      '<path d="M8 7H5a2 2 0 0 0-2 2v3a2 2 0 0 0 2 2h1v1a2 2 0 0 1-2 2H4M17 7h-3a2 2 0 0 0-2 2v3a2 2 0 0 0 2 2h1v1a2 2 0 0 1-2 2h-1"/>',
    chart:
      '<path d="M4 20V10M10 20V4M16 20v-7M22 20H2"/>',
  };

  const icon = (name, size = 20) =>
    `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${ICONS[name] || ""}</svg>`;

  const GITHUB_SVG =
    '<svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true"><path d="M12 .5A11.5 11.5 0 0 0 .5 12a11.5 11.5 0 0 0 7.86 10.92c.58.11.79-.25.79-.56v-2c-3.2.7-3.88-1.37-3.88-1.37-.53-1.35-1.3-1.71-1.3-1.71-1.06-.72.08-.71.08-.71 1.17.08 1.79 1.2 1.79 1.2 1.04 1.79 2.73 1.27 3.4.97.1-.76.4-1.27.74-1.56-2.56-.29-5.26-1.28-5.26-5.7 0-1.26.45-2.29 1.19-3.1-.12-.29-.52-1.46.11-3.05 0 0 .97-.31 3.18 1.18a11 11 0 0 1 5.8 0c2.2-1.5 3.17-1.18 3.17-1.18.63 1.59.23 2.76.12 3.05.74.81 1.18 1.84 1.18 3.1 0 4.43-2.7 5.4-5.28 5.69.42.36.79 1.07.79 2.16v3.2c0 .31.21.68.8.56A11.5 11.5 0 0 0 23.5 12 11.5 11.5 0 0 0 12 .5z"/></svg>';

  class LithonNavbar extends HTMLElement {
    connectedCallback() {
      const announcement =
        this.getAttribute("announcement") ||
        'Lithon is in early design — <a href="https://github.com" target="_blank" rel="noopener">follow progress on GitHub</a>.';
      const version = this.getAttribute("version") || "0.1.0";
      const links = (
        this.getAttribute("links") ||
        "Install:#install,Docs:#docs,Packages:#packages,Releases:#releases,Community:#community"
      )
        .split(",")
        .map((pair) => {
          const [label, href] = pair.split(":").map((s) => s.trim());
          return `<a href="${href}">${label}</a>`;
        })
        .join("");

      this.innerHTML = `
        <div class="announcement">${announcement}</div>
        <header class="navbar">
          <div class="navbar-inner">
            <a class="brand" href="#">
              <span class="brand-mark">Lithon</span>
              <img class="brand-mark-img" src="../../assets/boa.png" alt="" width="26" height="26" />
            </a>
            <nav class="nav-links" id="nav-links" aria-label="Primary">${links}</nav>
            <div class="nav-actions">
              <span class="version-pill">${version} ▾</span>
              <button class="icon-btn" id="theme-toggle" type="button" aria-label="Toggle dark mode">
                <span class="icon-moon">${icon("moon", 18)}</span>
                <span class="icon-sun">${icon("sun", 18)}</span>
              </button>
              <a class="icon-btn" href="https://github.com" target="_blank" rel="noopener" aria-label="GitHub">${GITHUB_SVG}</a>
              <button class="search-pill" type="button" id="search-btn">
                ${icon("search", 14)}<span>Search</span><kbd>Ctrl</kbd><kbd>K</kbd>
              </button>
              <button class="hamburger" id="hamburger" type="button" aria-label="Toggle menu" aria-expanded="false">
                <span></span><span></span><span></span>
              </button>
            </div>
          </div>
        </header>`;
    }
  }

  class LithonFooter extends HTMLElement {
    connectedCallback() {
      this.innerHTML = `
        <footer class="footer" id="community">
          <div class="footer-inner">
            <div class="footer-brand">
              <span class="brand-mark">Lithon</span>
              <p>Give Python wings.</p>
              <img class="footer-mascot" src="../../assets/boa.png" alt="Lithon cobra mascot" width="120" />
            </div>
            <nav class="footer-cols" aria-label="Footer">
              <div>
                <h4>Learn</h4>
                <a href="#what">What is Lithon?</a>
                <a href="#why">Why Lithon?</a>
                <a href="#compare">Compare</a>
              </div>
              <div>
                <h4>Project</h4>
                <a href="#use-cases">Use cases</a>
                <a href="#roadmap">Roadmap</a>
                <a href="#benchmark">Benchmarks</a>
              </div>
              <div>
                <h4>Community</h4>
                <a href="https://github.com" target="_blank" rel="noopener">GitHub</a>
                <a href="#install">Install</a>
                <a href="#community">Contributing</a>
              </div>
            </nav>
          </div>
          <div class="footer-bottom">
            <span>Early design — built with HTML, CSS &amp; JS.</span>
            <span>Palette: #474350 · #F8FFF4 · #FCFFEB · #FAFAC6 · #FECdaa</span>
          </div>
        </footer>`;
    }
  }

  class LithonQuicklink extends HTMLElement {
    connectedCallback() {
      const href = this.getAttribute("href") || "#";
      const ic = this.getAttribute("icon") || "clock";
      const title = this.getAttribute("title") || "";
      const desc = this.getAttribute("desc") || "";
      this.innerHTML = `
        <a class="quicklink" href="${href}">
          <span class="ql-icon">${icon(ic)}</span>
          <span class="ql-text"><strong>${title}</strong><span>${desc}</span></span>
        </a>`;
    }
  }

  class LithonCard extends HTMLElement {
    connectedCallback() {
      const ic = this.getAttribute("icon") || "grid";
      const title = this.getAttribute("title") || "";
      this.classList.add("card");
      this.innerHTML = `
        <span class="card-icon">${icon(ic, 24)}</span>
        <h3>${title}</h3>
        <p><slot></slot></p>`;
    }
  }

  class CodeWindow extends HTMLElement {
    connectedCallback() {
      const title = this.getAttribute("title") || "code";
      this.classList.add("code-window");
      this.innerHTML = `
        <div class="code-header">
          <span class="dot red"></span><span class="dot yellow"></span><span class="dot green"></span>
          <span class="code-title">${title}</span>
        </div>
        <pre><code><slot></slot></code></pre>`;
    }
  }

  class LithonSectionTitle extends HTMLElement {
    connectedCallback() {
      const sub = this.getAttribute("sub");
      this.innerHTML = `
        <h2 class="section-title"><slot></slot></h2>
        ${sub ? `<p class="section-sub">${sub}</p>` : ""}`;
    }
  }

  class LithonBadge extends HTMLElement {
    connectedCallback() {
      const variant = this.getAttribute("variant") || "neutral";
      this.innerHTML = `<span class="badge badge-${variant}"><slot></slot></span>`;
    }
  }

  class LithonQuote extends HTMLElement {
    connectedCallback() {
      this.classList.add("readme-quote");
      this.innerHTML = `<blockquote>${icon("quote", 22)}<p><slot></slot></p></blockquote>`;
    }
  }

  class CompareTable extends HTMLElement {
    connectedCallback() {
      const rows = [
        ["Syntax", "Python", "Python + annotations", "Python-like"],
        ["Typing", "Dynamic", "Optional", "<strong>Mandatory</strong>"],
        ["Speed on typed code", "🐢", "🚗", "🚀"],
        ["Silent type coercion", "✅ allowed", "⚠️ partial", "❌ never"],
        ["Runtime type guessing", "✅", "⚠️ partial", "❌ never"],
        ["Compiles to", "Bytecode", "C, then native", "Native machine code"],
        [
          "Philosophy",
          '"It\'ll figure itself out"',
          '"Speed up what you annotate"',
          '"Prove it, or it doesn\'t run"',
        ],
      ];
      this.classList.add("table-wrap");
      this.innerHTML = `
        <table class="compare-table">
          <thead>
            <tr><th></th><th>Python</th><th>Cython / mypyc</th><th class="col-lithon">Lithon</th></tr>
          </thead>
          <tbody>
            ${rows
              .map(
                (r) => `<tr>
                  <th scope="row">${r[0]}</th>
                  <td>${r[1]}</td>
                  <td>${r[2]}</td>
                  <td class="col-lithon">${r[3]}</td>
                </tr>`
              )
              .join("")}
          </tbody>
        </table>
        <p class="table-note">Lithon isn't trying to run your existing <code>.py</code> files unmodified.
        It's a stricter, sharper language that happens to speak Python's dialect — built for the code
        you'd <em>want</em> to be fast, not the code you happen to already have.</p>`;
    }
  }

  class UseCaseList extends HTMLElement {
    connectedCallback() {
      const cases = [
        ["bolt", "Numeric-heavy loops", "Simulations, signal processing, tight arithmetic that Python usually farms out to NumPy or C extensions."],
        ["gauge", "Performance-sensitive tooling", "Game logic, real-time data processing, anything where interpreter overhead is the bottleneck."],
        ["layers", "Data pipelines with predictable shapes", "Fixed schemas, known types, no need for Python's full dynamic flexibility."],
        ["flask", "Learning how compilers actually work", "A small, honest, from-scratch language for exploring static typing and native codegen."],
        ["tools", 'Python, but it has to be fast and correct', 'Where correctness and speed matter more than running every existing library.'],
      ];
      this.classList.add("cards");
      this.innerHTML = cases
        .map(
          ([ic, title, desc]) =>
            `<lithon-card icon="${ic}" title="${title}">${desc}</lithon-card>`
        )
        .join("");
    }
  }

  class PhaseCard extends HTMLElement {
    connectedCallback() {
      const tag = this.getAttribute("tag") || "";
      const title = this.getAttribute("title") || "";
      const done = this.hasAttribute("done");
      const current = this.hasAttribute("current");
      this.classList.add("phase");
      if (current) this.classList.add("current");
      this.innerHTML = `
        <span class="phase-tag ${done ? "done" : ""}">${tag}</span>
        <h3>${title}</h3>
        <p><slot></slot></p>`;
    }
  }

  class BenchmarkCard extends HTMLElement {
    connectedCallback() {
      this.classList.add("benchmark");
      this.innerHTML = `
        <div class="benchmark-head">
          <span class="benchmark-status">PASS</span>
          <span class="benchmark-note">Last updated by Lithon Reporter Mamba.</span>
        </div>
        <div class="table-wrap">
          <table class="compare-table bench-table">
            <thead>
              <tr><th>Benchmark</th><th>Lithon JIT</th><th>Reference</th><th>Speedup</th><th>Result</th></tr>
            </thead>
            <tbody>
              <tr>
                <th scope="row">fib(30)</th>
                <td>3.5727 ms</td>
                <td>757.2927 ms</td>
                <td class="col-lithon">212.0×</td>
                <td>832040</td>
              </tr>
            </tbody>
          </table>
        </div>`;
    }
  }

  customElements.define("lithon-navbar", LithonNavbar);
  customElements.define("lithon-footer", LithonFooter);
  customElements.define("lithon-quicklink", LithonQuicklink);
  customElements.define("lithon-card", LithonCard);
  customElements.define("code-window", CodeWindow);
  customElements.define("lithon-section-title", LithonSectionTitle);
  customElements.define("lithon-badge", LithonBadge);
  customElements.define("lithon-quote", LithonQuote);
  customElements.define("compare-table", CompareTable);
  customElements.define("use-case-list", UseCaseList);
  customElements.define("phase-card", PhaseCard);
  customElements.define("benchmark-card", BenchmarkCard);
})();
