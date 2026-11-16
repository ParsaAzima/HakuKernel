# Project site

Static HTML and CSS for the Haku project page. No JavaScript, no web fonts, no
CDN: every page loads one local stylesheet, one SVG icon and one small PNG icon
set, so the site works from a local HTTP server and from GitHub Pages
unchanged.

```
site/
  index.html            overview, boot log, ports table, limits, FAQ, quick start
  build.html            prerequisites, build targets, the four CTest gates, images, CI
  docs.html             documentation index, project pages, conventions
  404.html              not-found page (noindex)
  style.css             one stylesheet for all pages, light and dark
  favicon.svg           the mark
  apple-touch-icon.png  180x180 home-screen icon
  og.png                1200x630 social preview image
  robots.txt            crawl policy and sitemap location
  sitemap.xml           the three indexable URLs
```

Preview locally:

```cmd
python -m http.server -d site 8000
```

Then open `http://localhost:8000/`. Opening `site/index.html` directly in a
browser also works; all internal links are relative.

## SEO

The site is written for indexing first, then for reading:

- one `<h1>` per page and a keyword-bearing heading hierarchy;
- a unique title and description per page, a canonical URL, a robots directive
  and Open Graph/Twitter card metadata;
- JSON-LD (`WebSite`, `SoftwareSourceCode`, `FAQPage`, `CollectionPage`,
  `BreadcrumbList`) that mirrors the visible text, including the FAQ answers;
- `robots.txt` plus `sitemap.xml`, and a custom `404.html` marked `noindex`
  that uses absolute URLs (Pages serves it at the requested path, where
  relative assets would not resolve);
- no client-side rendering, no external requests, so the text is all in the
  HTML that a crawler receives.

A project Pages URL is a subdirectory of the host, so crawlers read
`robots.txt` at `https://parsaazima.github.io/robots.txt`, not at
`/HakuKernel/robots.txt`. Submit `sitemap.xml` in Search Console (or move to a
custom domain) to make the crawl policy authoritative; the sitemap itself is
absolute and works either way.

The canonical base is the GitHub Pages project URL,
`https://parsaazima.github.io/HakuKernel/`. If the site moves to a custom
domain, update every `<link rel="canonical">`, `og:url`, `og:image`,
`twitter:image`, the JSON-LD `@id`/`url` values, `robots.txt` and
`sitemap.xml` in one pass.

Publishing is handled by `.github/workflows/pages.yml`, which uploads this
directory as the Pages artifact on every push that touches `site/`. The
repository must have **Settings → Pages → Source** set to **GitHub Actions**
once; after that the workflow publishes to the canonical URL above.

Site content is part of the repository's accuracy rule: describe the
implemented subset, link the document that owns each claim, and do not add a
capability the boot-log markers do not prove.
