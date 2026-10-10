#!/usr/bin/env python3
"""Build the printable AetherSDR manual (PDF) from the Docusaurus docs.

The pages in docs/user/docs are concatenated in sidebar order into one
Markdown book: each sidebar category is a chapter, each page a section. The
book is converted to Typst by Pandoc (with tools/docs/pdf/filters.lua) and
typeset by Typst with the template in tools/docs/pdf/manual.typ.

    python3 tools/docs/build_pdf.py                       # US Letter
    python3 tools/docs/build_pdf.py --paper a4 --version v26.10.1 \\
        --output AetherSDR-Manual-v26.10.1-A4.pdf

Requirements: node (to read sidebars.js), pandoc >= 3.6, typst >= 0.13. Their
paths can be given with --pandoc / --typst, or the PANDOC / TYPST environment
variables. Only Typst's embedded fonts and the vendored tools/docs/pdf/fonts/
are used, so the output does not depend on the fonts installed on the build
machine.

Links are rewritten for paper: a link to another page or heading becomes an
in-document cross-reference (Typst prints its page number), and a link to a
web page keeps its text and puts the URL in a footnote. Every heading gets an
explicit, page-prefixed label so that headings repeated across pages
("Setup", "Troubleshooting") never collide; a cross-reference that does not
resolve is a build error, as it is for the Docusaurus build.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unicodedata
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SITE = REPO / "docs" / "user"
DOCS = SITE / "docs"
STATIC = SITE / "static"
PDF_DIR = Path(__file__).resolve().parent / "pdf"
SITE_URL = "https://docs.aethersdr.com"

# Pages that are linked from the book but are not in the sidebar are printed
# in an appendix, except these (contributor-only pages); links to them become
# plain text.
APPENDIX_EXCLUDE = {"docs-style-guide"}
APPENDIX_TITLE = "Additional Pages"
# A top-level sidebar doc is a chapter of its own, titled by its page title
# unless renamed here.
CHAPTER_TITLES = {"index": "Introduction"}

# Any indent: fences nested in list items are indented by the list marker.
FENCE_RE = re.compile(r"^(\s*)(`{3,}|~{3,})")
HEADING_RE = re.compile(r"^(#{1,6})[ \t]+(.*?)(?:[ \t]+#+)?[ \t]*$")
EXPLICIT_ID_RE = re.compile(r"\s*\{#([^}\s]+)\}\s*$")
# [text](target) and ![alt](target); the text may hold one level of brackets.
LINK_RE = re.compile(r"(!?)\[((?:[^\[\]\\]|\\.|\[[^\[\]]*\])*)\]\(\s*<?([^)\s>]+)>?(\s+\"[^\"]*\")?\s*\)")
ADMONITION_OPEN_RE = re.compile(r"^:::(\w+)(?:\[(.*)\])?\s*$")
# A screenshot embedded by tools/docs/embed_screenshots.py as a sized HTML
# <img> (the PDF is built with raw HTML off, so it becomes a Markdown image).
HTML_IMG_RE = re.compile(r'^\s*<img src="([^"]+)"[^>]*?\balt="([^"]*)"[^>]*/?>\s*$')


def html_img_to_markdown(line: str) -> str:
    m = HTML_IMG_RE.match(line)
    if not m:
        return line
    alt = (m.group(2).replace("&quot;", '"').replace("&lt;", "<").replace("&gt;", ">")
           .replace("&amp;", "&"))
    alt = alt.replace("\\", "\\\\").replace("[", "\\[").replace("]", "\\]")
    return f"![{alt}]({m.group(1)})"


def fail(msg: str) -> None:
    print(f"build_pdf: error: {msg}", file=sys.stderr)
    sys.exit(1)


# --------------------------------------------------------------------------
# Docs model


def parse_front_matter(text: str) -> tuple[dict, str]:
    """Split simple `key: value` YAML front matter from the body."""
    if not text.startswith("---\n"):
        return {}, text
    end = text.find("\n---\n", 4)
    if end < 0:
        return {}, text
    meta: dict[str, str] = {}
    for line in text[4:end].splitlines():
        m = re.match(r"^([A-Za-z_][\w-]*):\s*(.*)$", line)
        if not m:
            continue
        value = m.group(2).strip()
        if value.startswith('"') and value.endswith('"') and len(value) >= 2:
            try:
                value = json.loads(value)
            except json.JSONDecodeError:
                value = value[1:-1]
        elif value.startswith("'") and value.endswith("'") and len(value) >= 2:
            value = value[1:-1].replace("''", "'")
        meta[m.group(1)] = value
    return meta, text[end + 5:]


class Page:
    def __init__(self, path: Path):
        self.path = path
        meta, body = parse_front_matter(path.read_text(encoding="utf-8"))
        rel = path.relative_to(DOCS).with_suffix("").as_posix()
        self.doc_id = meta.get("id", rel)
        if "id" in meta and "/" in rel:
            self.doc_id = rel.rsplit("/", 1)[0] + "/" + meta["id"]
        self.title = meta.get("title") or self.doc_id
        self.meta = meta
        self.body = body
        # Label prefix: a valid Typst label, unique per page.
        self.label = "p-" + re.sub(r"[^A-Za-z0-9_-]", "-", self.doc_id)


def load_pages() -> dict[str, Page]:
    pages = {}
    for path in sorted(DOCS.rglob("*.md")):
        page = Page(path)
        pages[page.doc_id] = page
    return pages


def read_sidebar(node: str) -> list:
    script = (
        "import(process.argv[1]).then(m => "
        "process.stdout.write(JSON.stringify(m.default)))"
        ".catch(e => { console.error(e); process.exit(1); });"
    )
    url = (SITE / "sidebars.js").resolve().as_uri()
    try:
        out = subprocess.run([node, "-e", script, url], check=True,
                             capture_output=True, text=True)
    except FileNotFoundError:
        fail(f"node not found ({node}); it is needed to read sidebars.js")
    except subprocess.CalledProcessError as e:
        fail(f"could not read sidebars.js:\n{e.stderr}")
    sidebars = json.loads(out.stdout)
    if "docs" in sidebars:
        return sidebars["docs"]
    return next(iter(sidebars.values()))


def flatten_sidebar(items: list) -> list[tuple[str | None, list[str]]]:
    """Return [(chapter_title or None, [doc ids])].

    A top-level doc is a chapter by itself (title None: use the page title).
    A category is a chapter; nested categories are flattened into it.
    External `link` entries have no page and are skipped.
    """
    chapters: list[tuple[str | None, list[str]]] = []

    def doc_ids(entries: list) -> list[str]:
        ids: list[str] = []
        for entry in entries:
            if isinstance(entry, str):
                ids.append(entry)
            elif entry.get("type") == "doc":
                ids.append(entry["id"])
            elif entry.get("type") == "category":
                link = entry.get("link") or {}
                if link.get("type") == "doc":
                    ids.append(link["id"])
                ids.extend(doc_ids(entry.get("items", [])))
        return ids

    for entry in items:
        if isinstance(entry, str) or entry.get("type") == "doc":
            chapters.append((None, doc_ids([entry])))
        elif entry.get("type") == "category":
            chapters.append((entry["label"], doc_ids([entry])))
    return chapters


# --------------------------------------------------------------------------
# Headings and anchors (github-slugger, as Docusaurus uses)


def heading_plain_text(text: str) -> str:
    text = EXPLICIT_ID_RE.sub("", text)
    text = re.sub(r"!\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"<[^>]+>", "", text)
    text = text.replace("`", "")
    text = re.sub(r"(\*\*|__|\*)", "", text)
    text = re.sub(r"(?<![\w])_(.+?)_(?![\w])", r"\1", text)
    text = text.replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">")
    return text.strip()


def github_slug(text: str) -> str:
    out = []
    for ch in text.lower():
        cat = unicodedata.category(ch)
        if ch in " -" or cat[0] in "LMN" or cat == "Pc":
            out.append("-" if ch == " " else ch)
    return "".join(out)


class Slugger:
    def __init__(self):
        self.seen: dict[str, int] = {}

    def slug(self, text: str) -> str:
        base = github_slug(text)
        slug = base
        while slug in self.seen:
            self.seen[base] += 1
            slug = f"{base}-{self.seen[base]}"
        self.seen.setdefault(slug, 0)
        return slug


def iter_lines_outside_code(lines: list[str]):
    """Yield (index, line, in_code) with fenced-code tracking."""
    fence = None
    for i, line in enumerate(lines):
        m = FENCE_RE.match(line)
        if fence is None and m:
            fence = m.group(2)[0] * len(m.group(2))
            yield i, line, True
            continue
        if fence is not None:
            if m and m.group(2).startswith(fence) and line.strip() == m.group(2):
                fence = None
            yield i, line, True
            continue
        yield i, line, False


def collect_anchors(page: Page) -> dict[str, str]:
    """Docusaurus anchor -> book label, for every heading of the page."""
    anchors: dict[str, str] = {}
    slugger = Slugger()
    for _, line, in_code in iter_lines_outside_code(page.body.splitlines()):
        if in_code:
            continue
        m = HEADING_RE.match(line)
        if not m:
            continue
        explicit = EXPLICIT_ID_RE.search(m.group(2))
        anchor = explicit.group(1) if explicit else slugger.slug(heading_plain_text(m.group(2)))
        anchors[anchor] = f"{page.label}--{anchor}"
    return anchors


# --------------------------------------------------------------------------
# Page conversion


class Book:
    def __init__(self, pages: dict[str, Page], included: list[str]):
        self.pages = pages
        self.included = set(included)
        self.anchors = {doc_id: collect_anchors(pages[doc_id]) for doc_id in included}
        self.unresolved: list[str] = []
        self.dropped_links: list[str] = []

    def resolve_target(self, page: Page, target: str) -> str | None:
        """Return '#label' for an internal target, the target itself for an
        external one, or None when it points at a page not in the book."""
        # A file served by the site (pathname:///Manual.pdf) or a site page
        # outside the docs (/log-analyzer) is a web link on paper.
        if target.startswith("pathname:///"):
            return SITE_URL + target[len("pathname://"):]
        if target.startswith("/") and not target.startswith("//"):
            return SITE_URL + target
        if re.match(r"^[a-z][a-z0-9+.-]*:", target, re.I) or target.startswith("//"):
            return target
        path_part, _, anchor = target.partition("#")
        if path_part:
            if not path_part.endswith((".md", ".mdx")):
                return target  # an asset (/img/...) or a site route; handled later
            dest = (page.path.parent / path_part).resolve()
            try:
                dest_id = dest.relative_to(DOCS.resolve()).with_suffix("").as_posix()
            except ValueError:
                dest_id = None
            # front matter ids
            for doc_id, p in self.pages.items():
                if p.path.resolve() == dest:
                    dest_id = doc_id
                    break
            if dest_id not in self.included:
                self.dropped_links.append(f"{page.doc_id} -> {target}")
                return None
        else:
            dest_id = page.doc_id
        dest = self.pages[dest_id]
        if not anchor:
            return f"#{dest.label}"
        label = self.anchors[dest_id].get(anchor)
        if label is None:
            self.unresolved.append(f"{page.doc_id} -> {target}")
            return f"#{dest.label}"
        return f"#{label}"

    def rewrite_links(self, page: Page, line: str) -> str:
        # Leave links inside inline code alone.
        parts = re.split(r"(`+[^`]*`+)", line)
        for i in range(0, len(parts), 2):
            parts[i] = LINK_RE.sub(lambda m: self._link(page, m), parts[i])
        return "".join(parts)

    def _link(self, page: Page, m: re.Match) -> str:
        bang, text, target, title = m.group(1), m.group(2), m.group(3), m.group(4) or ""
        if bang:
            if target.startswith("/"):
                target = "static" + target  # Docusaurus serves static/ at /
            return f"![{text}]({target}{title})"
        resolved = self.resolve_target(page, target)
        if resolved is None:
            return text
        return f"[{text}]({resolved}{title})"

    def convert(self, page: Page, level: int, title: str | None = None) -> str:
        """Return the page as Markdown with its title at `level`.

        A page that is a chapter by itself (level 1) keeps its sections out
        of the numbering and the table of contents.
        """
        classes = " .unnumbered .unlisted" if level == 1 else ""
        shift = level  # page '##' (2) becomes level+1
        out = [f"{'#' * level} {title or page.title} {{#{page.label}}}", ""]
        slugger_anchors = iter(self.anchors[page.doc_id].values())
        lines = page.body.splitlines()
        for _, line, in_code in iter_lines_outside_code(lines):
            if in_code:
                out.append(line)
                continue
            m = HEADING_RE.match(line)
            if m:
                depth = min(len(m.group(1)) + shift - 1, 6)
                text = EXPLICIT_ID_RE.sub("", m.group(2))
                text = self.rewrite_links(page, text)
                out.append(f"{'#' * depth} {text} {{#{next(slugger_anchors)}{classes}}}")
                continue
            a = ADMONITION_OPEN_RE.match(line)
            if a:
                kind, title = a.group(1), (a.group(2) or "").replace('"', "'")
                out.append(f'::: {{.admonition .{kind} title="{title}"}}')
                continue
            out.append(self.rewrite_links(page, html_img_to_markdown(line)))
        out.append("")
        return "\n".join(out)


def linked_orphans(book_ids: list[str], pages: dict[str, Page]) -> list[str]:
    """Pages outside the sidebar that the book links to (transitively)."""
    included = set(book_ids)
    queue = list(book_ids)
    extra: list[str] = []
    while queue:
        page = pages[queue.pop(0)]
        for _, line, in_code in iter_lines_outside_code(page.body.splitlines()):
            if in_code:
                continue
            for m in LINK_RE.finditer(line):
                target = m.group(3).partition("#")[0]
                if m.group(1) or not target.endswith((".md", ".mdx")) or "://" in target:
                    continue
                dest = (page.path.parent / target).resolve()
                for doc_id, p in pages.items():
                    if p.path.resolve() == dest and doc_id not in included \
                            and doc_id not in APPENDIX_EXCLUDE:
                        included.add(doc_id)
                        extra.append(doc_id)
                        queue.append(doc_id)
    return extra


def build_markdown(pages: dict[str, Page], chapters, appendix: list[str]) -> tuple[str, Book]:
    included = list(dict.fromkeys([d for _, ids in chapters for d in ids] + appendix))
    missing = [d for d in included if d not in pages]
    if missing:
        fail(f"sidebar refers to missing pages: {', '.join(missing)}")
    book = Book(pages, included)
    parts: list[str] = []
    printed: dict[str, int] = {}
    chapter_slugs = Slugger()  # two categories may share a label
    for title, ids in chapters:
        if title is None and len(ids) == 1:
            parts.append(book.convert(pages[ids[0]], 1, CHAPTER_TITLES.get(ids[0])))
            continue
        chapter_label = "ch-" + chapter_slugs.slug(title or ids[0])
        parts.append(f"# {title} {{#{chapter_label}}}\n")
        for doc_id in ids:
            if doc_id in printed:
                # A page listed in two sidebar categories is printed once;
                # the second listing points at it.
                page = pages[doc_id]
                printed[doc_id] += 1
                parts.append(f"## {page.title} {{#{page.label}-also-{printed[doc_id]} .unnumbered}}\n\n"
                             f"This page is printed in full in another chapter: "
                             f"[{page.title}](#{page.label}).\n")
                continue
            printed[doc_id] = 1
            parts.append(book.convert(pages[doc_id], 2))
    if appendix:
        parts.append("```{=typst}\n#show: appendix\n```\n")
        parts.append(f"# {APPENDIX_TITLE} {{#ch-appendix}}\n")
        for doc_id in appendix:
            parts.append(book.convert(pages[doc_id], 2))
    return "\n".join(parts), book


# --------------------------------------------------------------------------
# Tools


def find_tool(name: str, explicit: str | None) -> str:
    candidate = explicit or os.environ.get(name.upper()) or shutil.which(name)
    if not candidate:
        fail(f"{name} not found; install it or pass --{name}")
    return candidate


def describe_version() -> str:
    try:
        out = subprocess.run(["git", "describe", "--tags", "--match", "v[0-9]*",
                              "--always", "--dirty"],
                             cwd=REPO, check=True, capture_output=True, text=True)
        return out.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "development build"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--output", "-o", type=Path,
                    default=SITE / "build" / "AetherSDR-Manual.pdf")
    ap.add_argument("--version", help="version printed on the cover "
                    "(default: git describe)")
    ap.add_argument("--date", help="build date on the cover (default: today, UTC)")
    ap.add_argument("--paper", default="us-letter", choices=["us-letter", "a4"])
    ap.add_argument("--pandoc")
    ap.add_argument("--typst")
    ap.add_argument("--node", default=os.environ.get("NODE", "node"))
    ap.add_argument("--keep-work", type=Path,
                    help="write the intermediate Markdown and Typst here and keep them")
    args = ap.parse_args()

    pandoc = find_tool("pandoc", args.pandoc)
    typst = find_tool("typst", args.typst)
    version = args.version or describe_version()
    now = _dt.datetime.now(_dt.timezone.utc)
    date = args.date or f"{now.day} {now:%B %Y}"

    pages = load_pages()
    chapters = flatten_sidebar(read_sidebar(args.node))
    sidebar_ids = list(dict.fromkeys(d for _, ids in chapters for d in ids))
    appendix = linked_orphans(sidebar_ids, pages)
    markdown, book = build_markdown(pages, chapters, appendix)

    if book.unresolved:
        fail("cross-references that do not resolve to a heading:\n  "
             + "\n  ".join(book.unresolved))
    for link in book.dropped_links:
        print(f"build_pdf: note: link to a page not in the manual printed as text: {link}",
              file=sys.stderr)
    unlisted = sorted(set(pages) - set(sidebar_ids) - set(appendix))
    if unlisted:
        print(f"build_pdf: note: not in the manual (not in the sidebar): {', '.join(unlisted)}",
              file=sys.stderr)

    if args.keep_work:
        work = args.keep_work.resolve()
        work.mkdir(parents=True, exist_ok=True)
        cleanup = None
    else:
        cleanup = tempfile.TemporaryDirectory(prefix="aethersdr-pdf-")
        work = Path(cleanup.name)
    try:
        (work / "manual.md").write_text(markdown, encoding="utf-8")
        for f in PDF_DIR.iterdir():
            if f.is_file() and f.suffix in (".typ", ".png", ".svg", ".jpg"):
                shutil.copy2(f, work / f.name)
        logo = REPO / "docs" / "assets" / "logo-circle.png"
        if not logo.exists():
            logo = STATIC / "img" / "logo.png"
        shutil.copy2(logo, work / "logo.png")
        if STATIC.is_dir():
            shutil.copytree(STATIC, work / "static", dirs_exist_ok=True)

        subprocess.run([
            pandoc, str(work / "manual.md"),
            "--from", "commonmark_x-raw_html",
            "--to", "typst",
            "--standalone",
            "--template", str(PDF_DIR / "template.typst"),
            "--lua-filter", str(PDF_DIR / "filters.lua"),
            "--resource-path", str(work),
            "--metadata", "title=AetherSDR User Manual",
            "--output", str(work / "main.typ"),
        ], check=True)

        args.output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([
            typst, "compile",
            "--root", str(work),
            "--ignore-system-fonts",
            "--font-path", str(PDF_DIR / "fonts"),
            "--input", f"version={version}",
            "--input", f"date={date}",
            "--input", f"paper={args.paper}",
            str(work / "main.typ"), str(args.output.resolve()),
        ], check=True)
    except subprocess.CalledProcessError as e:
        fail(f"{Path(e.cmd[0]).name} failed (exit {e.returncode})")
    finally:
        if cleanup:
            cleanup.cleanup()

    size = args.output.stat().st_size
    print(f"build_pdf: wrote {args.output} ({size / 1024 / 1024:.1f} MiB, "
          f"{len(sidebar_ids) + len(appendix)} pages of docs, version {version})")


if __name__ == "__main__":
    main()
