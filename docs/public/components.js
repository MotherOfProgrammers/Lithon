(() => {
  const ICONS = {
    clock: '<path d="M3 12a9 9 0 1 0 18 0a9 9 0 0 0 -18 0" /><path d="M12 7v5l3 3" />',
    sparkle:
      '<path d="M16 18a2 2 0 0 1 2 2a2 2 0 0 1 2 -2a2 2 0 0 1 -2 -2a2 2 0 0 1 -2 2m0 -12a2 2 0 0 1 2 2a2 2 0 0 1 2 -2a2 2 0 0 1 -2 -2a2 2 0 0 1 -2 2m-7 12a6 6 0 0 1 6 -6a6 6 0 0 1 -6 -6a6 6 0 0 1 -6 6a6 6 0 0 1 6 6" />',
    map: '<path d="M3 7l6 -3l6 3l6 -3v13l-6 3l-6 -3l-6 3v-13" /><path d="M9 4v13" /><path d="M15 7v13" />',
    github:
      '<path d="M9 19c-4.3 1.4 -4.3 -2.5 -6 -3m12 5v-3.5c0 -1 .1 -1.4 -.5 -2c2.8 -.3 5.5 -1.4 5.5 -6a4.6 4.6 0 0 0 -1.3 -3.2a4.2 4.2 0 0 0 -.1 -3.2s-1.1 -.3 -3.5 1.3a12.3 12.3 0 0 0 -6.2 0c-2.4 -1.6 -3.5 -1.3 -3.5 -1.3a4.2 4.2 0 0 0 -.1 3.2a4.6 4.6 0 0 0 -1.3 3.2c0 4.6 2.7 5.7 5.5 6c-.6 .6 -.6 1.2 -.5 2v3.5" />',
    grid:
      '<path d="M4 5a1 1 0 0 1 1 -1h4a1 1 0 0 1 1 1v4a1 1 0 0 1 -1 1h-4a1 1 0 0 1 -1 -1l0 -4" /><path d="M14 5a1 1 0 0 1 1 -1h4a1 1 0 0 1 1 1v4a1 1 0 0 1 -1 1h-4a1 1 0 0 1 -1 -1l0 -4" /><path d="M4 15a1 1 0 0 1 1 -1h4a1 1 0 0 1 1 1v4a1 1 0 0 1 -1 1h-4a1 1 0 0 1 -1 -1l0 -4" /><path d="M14 15a1 1 0 0 1 1 -1h4a1 1 0 0 1 1 1v4a1 1 0 0 1 -1 1h-4a1 1 0 0 1 -1 -1l0 -4" />',
    bolt: '<path d="M13 3l0 7l6 0l-8 11l0 -7l-6 0l8 -11" />',
    shield:
      '<path d="M11.46 20.846a12 12 0 0 1 -7.96 -14.846a12 12 0 0 0 8.5 -3a12 12 0 0 0 8.5 3a12 12 0 0 1 -.09 7.06" /><path d="M15 19l2 2l4 -4" />',
    book: '<path d="M3 19a9 9 0 0 1 9 0a9 9 0 0 1 9 0" /><path d="M3 6a9 9 0 0 1 9 0a9 9 0 0 1 9 0" /><path d="M3 6l0 13" /><path d="M12 6l0 13" /><path d="M21 6l0 13" />',
    rocket:
      '<path d="M4 13a8 8 0 0 1 7 7a6 6 0 0 0 3 -5a9 9 0 0 0 6 -8a3 3 0 0 0 -3 -3a9 9 0 0 0 -8 6a6 6 0 0 0 -5 3" /><path d="M7 14a6 6 0 0 0 -3 6a6 6 0 0 0 6 -3" /><path d="M14 9a1 1 0 1 0 2 0a1 1 0 1 0 -2 0" />',
    gauge:
      '<path d="M3 12a9 9 0 1 0 18 0a9 9 0 1 0 -18 0" /><path d="M11 12a1 1 0 1 0 2 0a1 1 0 1 0 -2 0" /><path d="M13.41 10.59l2.59 -2.59" /><path d="M7 12a5 5 0 0 1 5 -5" />',
    terminal:
      '<path d="M8 9l3 3l-3 3" /><path d="M13 15l3 0" /><path d="M3 6a2 2 0 0 1 2 -2h14a2 2 0 0 1 2 2v12a2 2 0 0 1 -2 2h-14a2 2 0 0 1 -2 -2l0 -12" />',
    flask:
      '<path d="M9 3l6 0" /><path d="M10 9l4 0" /><path d="M10 3v6l-4 11a.7 .7 0 0 0 .5 1h11a.7 .7 0 0 0 .5 -1l-4 -11v-6" />',
    layers:
      '<path d="M12 4l-8 4l8 4l8 -4l-8 -4" /><path d="M4 12l8 4l8 -4" /><path d="M4 16l8 4l8 -4" />',
    tools:
      '<path d="M7 10h3v-3l-3.5 -3.5a6 6 0 0 1 8 8l6 6a2 2 0 0 1 -3 3l-6 -6a6 6 0 0 1 -8 -8l3.5 3.5" />',
    sun: '<path d="M8 12a4 4 0 1 0 8 0a4 4 0 1 0 -8 0" /><path d="M3 12h1m8 -9v1m8 8h1m-9 8v1m-6.4 -15.4l.7 .7m12.1 -.7l-.7 .7m0 11.4l.7 .7m-12.1 -.7l-.7 .7" />',
    moon: '<path d="M12 3c.132 0 .263 0 .393 0a7.5 7.5 0 0 0 7.92 12.446a9 9 0 1 1 -8.313 -12.454l0 .008" />',
    search: '<path d="M3 10a7 7 0 1 0 14 0a7 7 0 1 0 -14 0" /><path d="M21 21l-6 -6" />',
    quote:
      '<path d="M10 11h-4a1 1 0 0 1 -1 -1v-3a1 1 0 0 1 1 -1h3a1 1 0 0 1 1 1v6c0 2.667 -1.333 4.333 -4 5" /><path d="M19 11h-4a1 1 0 0 1 -1 -1v-3a1 1 0 0 1 1 -1h3a1 1 0 0 1 1 1v6c0 2.667 -1.333 4.333 -4 5" />',
    chart:
      '<path d="M3 13a1 1 0 0 1 1 -1h4a1 1 0 0 1 1 1v6a1 1 0 0 1 -1 1h-4a1 1 0 0 1 -1 -1l0 -6" /><path d="M15 9a1 1 0 0 1 1 -1h4a1 1 0 0 1 1 1v10a1 1 0 0 1 -1 1h-4a1 1 0 0 1 -1 -1l0 -10" /><path d="M9 5a1 1 0 0 1 1 -1h4a1 1 0 0 1 1 1v14a1 1 0 0 1 -1 1h-4a1 1 0 0 1 -1 -1l0 -14" /><path d="M4 20h14" />',
    bulb: '<path d="M3 12h1m8 -9v1m8 8h1m-15.4 -6.4l.7 .7m12.1 -.7l-.7 .7" /><path d="M9 16a5 5 0 1 1 6 0a3.5 3.5 0 0 0 -1 3a2 2 0 0 1 -4 0a3.5 3.5 0 0 0 -1 -3" /><path d="M9.7 17l4.6 0" />',
    versus:
      '<path d="M4 6a2 2 0 1 0 4 0a2 2 0 1 0 -4 0" /><path d="M16 18a2 2 0 1 0 4 0a2 2 0 1 0 -4 0" /><path d="M11 6h5a2 2 0 0 1 2 2v8" /><path d="M14 9l-3 -3l3 -3" /><path d="M13 18h-5a2 2 0 0 1 -2 -2v-8" /><path d="M10 15l3 3l-3 3" />',
    target:
      '<path d="M11 12a1 1 0 1 0 2 0a1 1 0 1 0 -2 0" /><path d="M7 12a5 5 0 1 0 10 0a5 5 0 1 0 -10 0" /><path d="M3 12a9 9 0 1 0 18 0a9 9 0 1 0 -18 0" />',
    compass:
      '<path d="M8 16l2 -6l6 -2l-2 6l-6 2" /><path d="M3 12a9 9 0 1 0 18 0a9 9 0 1 0 -18 0" /><path d="M12 3l0 2" /><path d="M12 19l0 2" /><path d="M3 12l2 0" /><path d="M19 12l2 0" />',
    check: '<path d="M5 12l5 5l10 -10" />',
    alert:
      '<path d="M12 9v4" /><path d="M10.363 3.591l-8.106 13.534a1.914 1.914 0 0 0 1.636 2.871h16.214a1.914 1.914 0 0 0 1.636 -2.87l-8.106 -13.536a1.914 1.914 0 0 0 -3.274 0" /><path d="M12 16h.01" />',
    x: '<path d="M18 6l-12 12" /><path d="M6 6l12 12" />',
  };

  const icon = (name, size = 20) =>
    `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${ICONS[name] || ""}</svg>`;

  const GITHUB_SVG =
    '<svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true"><path d="M12 .5A11.5 11.5 0 0 0 .5 12a11.5 11.5 0 0 0 7.86 10.92c.58.11.79-.25.79-.56v-2c-3.2.7-3.88-1.37-3.88-1.37-.53-1.35-1.3-1.71-1.3-1.71-1.06-.72.08-.71.08-.71 1.17.08 1.79 1.2 1.79 1.2 1.04 1.79 2.73 1.27 3.4.97.1-.76.4-1.27.74-1.56-2.56-.29-5.26-1.28-5.26-5.7 0-1.26.45-2.29 1.19-3.1-.12-.29-.52-1.46.11-3.05 0 0 .97-.31 3.18 1.18a11 11 0 0 1 5.8 0c2.2-1.5 3.17-1.18 3.17-1.18.63 1.59.23 2.76.12 3.05.74.81 1.18 1.84 1.18 3.1 0 4.43-2.7 5.4-5.28 5.69.42.36.79 1.07.79 2.16v3.2c0 .31.21.68.8.56A11.5 11.5 0 0 0 23.5 12 11.5 11.5 0 0 0 12 .5z"/></svg>';

  class LithonNavbar extends HTMLElement {
    connectedCallback() {
      const announcement =
        this.getAttribute("announcement") ||
        'Lithon is in early design. <a href="https://github.com" target="_blank" rel="noopener">Follow progress on GitHub</a>.';
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
            <span>Early design. Built with HTML, CSS &amp; JS.</span>
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
      const content = this.innerHTML;
      const ic = this.getAttribute("icon") || "grid";
      const title = this.getAttribute("title") || "";
      this.classList.add("card");
      this.innerHTML = `
        <span class="card-icon">${icon(ic, 24)}</span>
        <h3>${title}</h3>
        <p>${content}</p>`;
    }
  }

  class CodeWindow extends HTMLElement {
    connectedCallback() {
      const content = this.innerHTML;
      const title = this.getAttribute("title") || "code";
      this.classList.add("code-window");
      this.innerHTML = `
        <div class="code-header">
          <span class="dot red"></span><span class="dot yellow"></span><span class="dot green"></span>
          <span class="code-title">${title}</span>
        </div>
        <pre><code>${content}</code></pre>`;
    }
  }

  class LithonSectionTitle extends HTMLElement {
    connectedCallback() {
      const content = this.innerHTML;
      const sub = this.getAttribute("sub");
      const ic = this.getAttribute("icon");
      this.innerHTML = `
        <h2 class="section-title">${ic ? `<span class="section-icon">${icon(ic, 32)}</span>` : ""}${content}</h2>
        ${sub ? `<p class="section-sub">${sub}</p>` : ""}`;
    }
  }

  class LithonBadge extends HTMLElement {
    connectedCallback() {
      const content = this.innerHTML;
      const variant = this.getAttribute("variant") || "neutral";
      this.innerHTML = `<span class="badge badge-${variant}">${content}</span>`;
    }
  }

  class LithonQuote extends HTMLElement {
    connectedCallback() {
      const content = this.innerHTML;
      this.classList.add("readme-quote");
      this.innerHTML = `<blockquote>${icon("quote", 22)}<p>${content}</p></blockquote>`;
    }
  }

  class CompareTable extends HTMLElement {
    connectedCallback() {
      const ok = `<span class="i-ok">${icon("check", 16)}</span>`;
      const warn = `<span class="i-warn">${icon("alert", 16)}</span>`;
      const no = `<span class="i-no">${icon("x", 16)}</span>`;
      const rows = [
        ["Syntax", "Python", "Python + annotations", "Python-like"],
        ["Typing", "Dynamic", "Optional", "<strong>Mandatory</strong>"],
        ["Speed on typed code", "Slow", "Fast", "<strong>Native</strong>"],
        ["Silent type coercion", `${ok} allowed`, `${warn} partial`, `${no} never`],
        ["Runtime type guessing", ok, `${warn} partial`, `${no} never`],
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
        <p class="table-note">Not a <code>.py</code> runner. A stricter language for the code you <em>want</em> to be fast.</p>`;
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
      const content = this.innerHTML;
      const tag = this.getAttribute("tag") || "";
      const title = this.getAttribute("title") || "";
      const done = this.hasAttribute("done");
      const current = this.hasAttribute("current");
      this.classList.add("phase");
      if (current) this.classList.add("current");
      this.innerHTML = `
        <span class="phase-tag ${done ? "done" : ""}">${tag}</span>
        <h3>${title}</h3>
        <p>${content}</p>`;
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
