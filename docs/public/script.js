(() => {
  const root = document.documentElement;
  const searchModal = document.getElementById("search-modal");
  const searchInput = document.getElementById("search-input");
  const searchResults = document.getElementById("search-results");

  const stored = localStorage.getItem("lithon-theme");
  if (stored) root.dataset.theme = stored;

  const onReady = () => {
    const themeToggle = document.getElementById("theme-toggle");
    const hamburger = document.getElementById("hamburger");
    const navLinks = document.getElementById("nav-links");
    const searchBtn = document.getElementById("search-btn");

    if (themeToggle) {
      themeToggle.addEventListener("click", () => {
        const next = root.dataset.theme === "dark" ? "light" : "dark";
        root.dataset.theme = next;
        localStorage.setItem("lithon-theme", next);
      });
    }

    if (hamburger && navLinks) {
      hamburger.addEventListener("click", () => {
        const open = navLinks.classList.toggle("open");
        hamburger.setAttribute("aria-expanded", String(open));
      });
      navLinks.addEventListener("click", (e) => {
        if (e.target.tagName === "A") {
          navLinks.classList.remove("open");
          hamburger.setAttribute("aria-expanded", "false");
        }
      });
    }

    if (searchBtn) searchBtn.addEventListener("click", openSearch);
  };

  if (customElements.get("lithon-navbar")) {
    customElements.whenDefined("lithon-navbar").then(onReady);
  } else {
    onReady();
  }

  const index = [
    { title: "Install", desc: "Get Lithon up and running", href: "#install" },
    { title: "What is Lithon?", desc: "The Python-flavored language built for speed", href: "#what" },
    { title: "Why Lithon?", desc: "Understand what Lithon solves", href: "#why" },
    { title: "How is this different?", desc: "Compare Python, Cython, and Lithon", href: "#compare" },
    { title: "Use cases", desc: "Where Lithon fits best", href: "#use-cases" },
    { title: "Roadmap", desc: "See what's coming next", href: "#roadmap" },
    { title: "Benchmarks", desc: "fib(30) and latest results", href: "#benchmark" },
    { title: "Community", desc: "Join the conversation", href: "#community" },
    { title: "Contributing", desc: "Learn how to contribute", href: "#community" },
    { title: "Type system", desc: "Mandatory static typing explained", href: "#compare" },
  ];

  let activeIndex = 0;

  function renderResults(query) {
    const q = query.trim().toLowerCase();
    const matches = q
      ? index.filter(
          (i) =>
            i.title.toLowerCase().includes(q) ||
            i.desc.toLowerCase().includes(q)
        )
      : index.slice(0, 6);

    activeIndex = 0;
    searchResults.innerHTML = matches
      .map(
        (m, i) => `
      <li>
        <a href="${m.href}" class="${i === 0 ? "active" : ""}">
          <div class="r-title">${m.title}</div>
          <div class="r-desc">${m.desc}</div>
        </a>
      </li>`
      )
      .join("");
  }

  function openSearch() {
    searchModal.hidden = false;
    renderResults("");
    searchInput.value = "";
    searchInput.focus();
  }

  function closeSearch() {
    searchModal.hidden = true;
  }

  document.addEventListener("keydown", (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "k") {
      e.preventDefault();
      openSearch();
      return;
    }
    if (searchModal.hidden) return;

    const items = [...searchResults.querySelectorAll("a")];
    if (e.key === "Escape") {
      closeSearch();
    } else if (e.key === "ArrowDown") {
      e.preventDefault();
      activeIndex = Math.min(activeIndex + 1, items.length - 1);
      items.forEach((el, i) => el.classList.toggle("active", i === activeIndex));
    } else if (e.key === "ArrowUp") {
      e.preventDefault();
      activeIndex = Math.max(activeIndex - 1, 0);
      items.forEach((el, i) => el.classList.toggle("active", i === activeIndex));
    } else if (e.key === "Enter" && items[activeIndex]) {
      items[activeIndex].click();
      closeSearch();
    }
  });

  searchModal.addEventListener("click", (e) => {
    if (e.target === searchModal) closeSearch();
  });

  searchInput.addEventListener("input", (e) => renderResults(e.target.value));

  searchResults.addEventListener("click", (e) => {
    if (e.target.closest("a")) closeSearch();
  });
})();
