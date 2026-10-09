// Layout of the printed AetherSDR manual.
//
// build_pdf.py passes these with `typst compile --input key=value`:
//   version  printed on the cover and in the PDF metadata
//   date     build date printed on the cover
//   paper    "us-letter" (default) or "a4"
//
// Only Typst's embedded fonts (Libertinus Serif, DejaVu Sans Mono) and the
// vendored fonts/ are used, with --ignore-system-fonts, so every machine
// produces the same book.

#let version = sys.inputs.at("version", default: "development build")
#let build-date = sys.inputs.at("date", default: "")
#let paper = sys.inputs.at("paper", default: "us-letter")

// AetherSDR blue (the docs site's light-mode primary, readable on white),
// used sparingly: chapter numbers, rules and box edges. Everything else is
// black on white so the book prints well in greyscale.
#let accent = rgb("#0a6aa8")
#let accent-light = rgb("#eaf3f9")
#let rule-grey = luma(190)
#let code-fill = luma(245)

// Noto Sans Symbols 2 (fonts/, SIL OFL) supplies the UI icons the docs
// quote, such as ☰, ✕ and 🔒, that the embedded fonts lack.
#let body-font = ("Libertinus Serif", "Noto Sans Symbols 2")
#let mono-font = ("DejaVu Sans Mono", "Noto Sans Symbols 2")

#let in-appendix = state("in-appendix", false)

#let heading-number(..nums) = {
  let n = nums.pos()
  if n.len() <= 3 { numbering("1.1", ..n) }
}

#let appendix-number(..nums) = {
  let n = nums.pos()
  if n.len() <= 3 { numbering("A.1", ..n) }
}

// Switches chapter numbering to letters; build_pdf.py emits `#show: appendix`
// before the first appendix chapter.
#let appendix(body) = {
  in-appendix.update(true)
  counter(heading).update(0)
  set heading(numbering: appendix-number)
  body
}

// Docusaurus admonitions (:::info[Status] and friends).
#let admonition(kind: "note", title: "", body) = {
  let edge = if kind in ("warning", "danger", "caution") { luma(60) } else { accent }
  let show-title = title != "" and title != "Status"
  block(
    width: 100%,
    breakable: true,
    fill: accent-light,
    stroke: (left: 2.5pt + edge),
    inset: (left: 10pt, right: 8pt, y: 7pt),
    radius: (right: 2pt),
    above: 1em,
    below: 1em,
  )[
    #set text(size: 0.92em)
    #set par(justify: false)
    #if show-title [
      #text(weight: "bold", fill: edge, title)
      #v(-0.3em)
    ]
    #body
  ]
}

#let running-header() = context {
  let page-no = here().page()
  // No header on a page that opens a chapter.
  if query(heading.where(level: 1)).any(h => h.location().page() == page-no) {
    return
  }
  let before = query(selector(heading.where(level: 1)).before(here()))
  if before.len() == 0 { return }
  let chapter = before.last()
  let label = if chapter.numbering != none {
    let word = if in-appendix.at(chapter.location()) { "Appendix" } else { "Chapter" }
    [#word #numbering(chapter.numbering, ..counter(heading).at(chapter.location()))]
  }
  set text(size: 8.5pt, fill: luma(80))
  grid(
    columns: (1fr, auto),
    align: (left, right),
    smallcaps[AetherSDR User Manual],
    [#label#if label != none [ · ]#chapter.body],
  )
  v(-0.45em)
  line(length: 100%, stroke: 0.4pt + rule-grey)
}

#let page-footer = context {
  set text(size: 9pt)
  align(center, counter(page).display(page.numbering))
}

#let cover() = {
  set page(header: none, footer: none, numbering: none)
  v(1fr)
  align(center)[
    #image("logo.png", width: 4.2cm)
    #v(1.2cm)
    #text(size: 34pt, weight: "bold", tracking: 0.02em)[AetherSDR]
    #v(0.1cm)
    #text(size: 18pt)[User Manual]
    #v(0.5cm)
    #line(length: 4cm, stroke: 1.2pt + accent)
    #v(0.5cm)
    #text(size: 12pt, style: "italic")[Native. Open. Yours.]
  ]
  v(1.2fr)
  align(center)[
    #set text(size: 11pt)
    #text(weight: "bold")[Version #version] \
    #if build-date != "" [Built #build-date \ ]
    #v(0.3cm)
    #text(size: 9.5pt, fill: luma(70))[`https://docs.aethersdr.com`]
  ]
  v(0.6fr)
}

#let colophon() = {
  set page(header: none, footer: none, numbering: none)
  set text(size: 9pt)
  set par(justify: false)
  v(1fr)
  [
    *AetherSDR User Manual* \
    Version #version#if build-date != "" [, built #build-date].

    This manual is generated from the online documentation at
    `https://docs.aethersdr.com`, which is always the most current version.
    The online pages are searchable and link to each other directly; in this
    book a cross-reference prints the number of the page it refers to, and
    a web address is printed in a footnote.

    AetherSDR is free software, licensed under the GNU General Public
    License v3.0. Source code, releases and issue tracking:
    `https://github.com/aethersdr/AetherSDR`.

    FlexRadio, SmartSDR and other product names are trademarks of their
    respective owners. AetherSDR is an independent project and is not
    affiliated with or endorsed by FlexRadio Systems.
  ]
}

#let manual(body) = {
  set document(
    title: [AetherSDR User Manual (#version)],
    author: "AetherSDR contributors",
    keywords: ("AetherSDR", "SDR", "FlexRadio", "amateur radio"),
  )
  set page(
    paper: paper,
    margin: (inside: 2.4cm, outside: 2.2cm, top: 2.6cm, bottom: 2.4cm),
    header-ascent: 35%,
  )
  set text(font: body-font, size: 10.5pt, lang: "en", region: "US",
           hyphenate: true, fallback: true)
  set par(justify: true, leading: 0.62em, spacing: 0.95em)
  set list(indent: 0.6em, body-indent: 0.5em, spacing: 0.65em)
  set enum(indent: 0.6em, body-indent: 0.5em, spacing: 0.65em)
  set terms(hanging-indent: 1.5em)
  set footnote.entry(separator: line(length: 30%, stroke: 0.4pt + rule-grey))
  show footnote.entry: set text(size: 8pt)
  show footnote.entry: set par(justify: false)

  // Block quotes (the docs use them for asides): a grey edge, no italics.
  show quote.where(block: true): it => block(
    width: 100%,
    stroke: (left: 1.5pt + rule-grey),
    inset: (left: 10pt, y: 2pt),
    above: 1em,
    below: 1em,
    it.body,
  )

  // Code.
  // Typst already sets raw text at 0.8em; inline code ends up at 0.88em.
  show raw: set text(font: mono-font)
  show raw.where(block: false): set text(size: 1.1em)
  show raw.where(block: true): it => block(
    width: 100%,
    fill: code-fill,
    stroke: 0.4pt + luma(215),
    inset: (x: 8pt, y: 6pt),
    radius: 2pt,
    breakable: true,
    above: 0.9em,
    below: 0.9em,
  )[#set text(size: 8.6pt); #set par(justify: false); #it]

  // Tables: compact, horizontal rules only, header repeats across pages.
  show figure.where(kind: table): set block(breakable: true)
  show figure.where(kind: table): set figure(supplement: none)
  show figure.where(kind: table): set figure.caption(position: top)
  set table(
    inset: (x: 5pt, y: 4pt),
    stroke: (x, y) => (
      top: if y == 0 { 0.8pt } else { 0pt },
      bottom: if y == 0 { 0.6pt } else { 0.3pt + rule-grey },
    ),
  )
  show table: set text(size: 8.8pt)
  show table: set align(left)
  show table: set par(justify: false)
  show table.cell.where(y: 0): set text(weight: "bold")
  show table: it => { v(0.3em); it; v(0.3em) }

  // Links: a cross-reference prints its page number; a bare web address is
  // set in the code font so it reads as an address.
  show link: it => {
    if type(it.dest) == label {
      it
      context {
        let target = query(it.dest)
        if target.len() > 0 {
          let p = counter(page).at(target.first().location()).first()
          text(fill: accent, size: 0.85em)[#sym.space.nobreak\(p.#sym.space.nobreak#p)]
        }
      }
    } else if type(it.dest) == str {
      text(font: mono-font, size: 0.85em, it)
    } else {
      it
    }
  }

  // Headings: chapters are level 1, docs pages level 2, their sections 3+.
  set heading(numbering: heading-number)
  show heading: set text(hyphenate: false)
  show heading: set par(justify: false)
  show heading.where(level: 1): it => {
    pagebreak(weak: true)
    counter(footnote).update(0)
    v(2.2cm)
    if it.numbering != none {
      let word = if in-appendix.get() { "Appendix" } else { "Chapter" }
      text(size: 11pt, weight: "bold", fill: accent, tracking: 0.12em,
           upper[#word #counter(heading).display(it.numbering)])
      v(0.2cm)
    }
    text(size: 26pt, weight: "bold", it.body)
    v(0.2cm)
    line(length: 100%, stroke: 1pt + accent)
    v(0.9cm)
  }
  show heading.where(level: 2): it => {
    v(1.4em, weak: true)
    block(sticky: true, below: 0.9em)[
      #set text(size: 17pt, weight: "bold")
      #if it.numbering != none [#text(fill: accent, counter(heading).display(it.numbering))#h(0.6em)]
      #it.body
    ]
  }
  show heading.where(level: 3): it => block(above: 1.4em, below: 0.7em, sticky: true)[
    #set text(size: 13pt, weight: "bold")
    #if it.numbering != none [#counter(heading).display(it.numbering)#h(0.5em)]
    #it.body
  ]
  show heading.where(level: 4): it => block(above: 1.2em, below: 0.6em, sticky: true,
    text(size: 11pt, weight: "bold", it.body))
  show heading.where(level: 5): it => block(above: 1.1em, below: 0.5em, sticky: true,
    text(size: 10.5pt, weight: "bold", style: "italic", it.body))
  show heading.where(level: 6): it => block(above: 1em, below: 0.5em, sticky: true,
    text(size: 10.5pt, style: "italic", it.body))

  // Front matter: cover, colophon, contents (roman page numbers).
  cover()
  pagebreak()
  colophon()
  pagebreak()

  set page(numbering: "i", footer: page-footer, header: none)
  counter(page).update(1)
  {
    show outline.entry.where(level: 1): it => {
      v(0.55em)
      text(weight: "bold", it)
    }
    set outline.entry(fill: repeat(gap: 0.35em)[.])
    outline(title: [Contents], depth: 2, indent: auto)
  }
  pagebreak(weak: true)

  set page(numbering: "1", footer: page-footer, header: running-header())
  counter(page).update(1)
  body
}
