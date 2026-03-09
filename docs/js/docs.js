/**
 * LoRaLink Documentation — Runtime Logic
 * Builds header, sidebar navigation, on-page TOC, and footer
 * from DOCS_CONFIG defined in config.js.
 */
(function () {
  "use strict";

  const C = window.DOCS_CONFIG || {};
  const BASE = C.baseUrl || ".";
  const currentPage =
    location.pathname.split("/").pop() || "index.html";

  /* ── Build Header ────────────────────────────────── */
  function buildHeader() {
    const hdr = document.createElement("header");
    hdr.className = "doc-header";
    hdr.innerHTML = `
      <div class="logo">
        <button class="hamburger" aria-label="Toggle menu">☰</button>
        <a href="${BASE}/index.html" style="text-decoration:none;display:flex;align-items:center;gap:0.75rem">
          <img src="${BASE}/media/logo.png" alt="${C.productShort || 'LoRaLink'}" style="height:28px">
          <span class="product-name">${C.productShort || "LoRaLink"}</span>
          <span class="version">${C.version || ""}</span>
        </a>
      </div>
      <div class="header-links">
        <a href="${C.companyUrl || "#"}" target="_blank">${C.companyName || ""}</a>
      </div>`;
    document.body.prepend(hdr);

    // Hamburger toggle
    hdr.querySelector(".hamburger").addEventListener("click", () => {
      document.querySelector(".doc-sidebar")?.classList.toggle("open");
    });
  }

  /* ── Build Sidebar ───────────────────────────────── */
  function buildSidebar() {
    const aside = document.createElement("aside");
    aside.className = "doc-sidebar";

    let navHtml = '<div class="sidebar-section">Documentation</div><nav>';
    (C.nav || []).forEach((item) => {
      const active = item.href === currentPage ? " active" : "";
      navHtml += `<a href="${BASE}/${item.href}" class="${active}">
        <span class="nav-icon">${item.icon || ""}</span>${item.title}</a>`;
    });
    navHtml += "</nav>";

    // On-page TOC placeholder (filled by buildPageTOC)
    navHtml += '<div class="page-toc" id="page-toc"></div>';

    navHtml += `<div class="sidebar-footer">${C.copyright || ""} · <a href="${C.companyUrl || "#"}" target="_blank">${C.companyName || ""}</a></div>`;

    aside.innerHTML = navHtml;
    document.body.insertBefore(aside, document.querySelector(".doc-main"));
  }

  /* ── Build On-Page TOC (scroll spy) ──────────────── */
  function buildPageTOC() {
    const container = document.getElementById("page-toc");
    if (!container) return;

    const headings = document.querySelectorAll(".doc-main h2, .doc-main h3");
    if (headings.length < 2) return; // Skip TOC for very short pages

    let html = '<div class="toc-title">On This Page</div>';
    headings.forEach((h) => {
      if (!h.id) h.id = h.textContent.trim().toLowerCase().replace(/[^a-z0-9]+/g, "-");
      const depth = h.tagName === "H3" ? " depth-3" : "";
      html += `<a href="#${h.id}" class="${depth}">${h.textContent}</a>`;
    });
    container.innerHTML = html;

    // Scroll spy
    const tocLinks = container.querySelectorAll("a");
    const observer = new IntersectionObserver(
      (entries) => {
        entries.forEach((e) => {
          if (e.isIntersecting) {
            tocLinks.forEach((l) => l.classList.remove("active"));
            const link = container.querySelector(`a[href="#${e.target.id}"]`);
            if (link) link.classList.add("active");
          }
        });
      },
      { rootMargin: "-80px 0px -60% 0px" }
    );
    headings.forEach((h) => observer.observe(h));
  }

  /* ── Build Footer ────────────────────────────────── */
  function buildFooter() {
    const footer = document.createElement("footer");
    footer.className = "doc-footer";
    footer.innerHTML = `
      <span>${C.copyright || ""}</span>
      <span>${C.productName || "LoRaLink"} ${C.version || ""}</span>`;
    document.body.appendChild(footer);
  }

  /* ── Close sidebar on link click (mobile) ────────── */
  function bindMobileClose() {
    document.querySelectorAll(".doc-sidebar nav a").forEach((a) => {
      a.addEventListener("click", () => {
        document.querySelector(".doc-sidebar")?.classList.remove("open");
      });
    });
  }

  /* ── Init ─────────────────────────────────────────── */
  function init() {
    buildHeader();
    buildSidebar();
    buildPageTOC();
    buildFooter();
    bindMobileClose();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
