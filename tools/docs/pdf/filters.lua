-- Pandoc filter for the printed AetherSDR manual (Typst output).
--
-- * Admonitions (`::: {.admonition .info title="Status"}`, written by
--   build_pdf.py from Docusaurus `:::info[Status]`) become #admonition boxes
--   defined in manual.typ.
-- * Web links keep their text and put the URL in a footnote; a link whose
--   text is already the URL is printed as-is. Inside headings and tables
--   the URL goes inline in parentheses instead (footnote marks in headings
--   leak into the table of contents and running headers).
-- * Internal links (#label) are left for manual.typ, which prints the page
--   number after them.

local stringify = pandoc.utils.stringify

-- Emoji that no bundled font has. Print a close symbol, or drop a purely
-- decorative one, rather than an empty box.
local GLYPH_SUBSTITUTES = {
  ['🎯'] = '◎',
  ['💡'] = '',
  ['💲'] = '',
}

local function substitute_glyphs(s)
  for from, to in pairs(GLYPH_SUBSTITUTES) do
    s = s:gsub(from, to)
  end
  return s
end

local function fix_text(el)
  local text = substitute_glyphs(el.text)
  if text ~= el.text then
    el.text = text
    return el
  end
end

local function typst(s)
  return pandoc.RawInline('typst', s)
end

local function typst_string(s)
  return '"' .. s:gsub('\\', '\\\\'):gsub('"', '\\"') .. '"'
end

local function is_external(target)
  return target:match('^[%a][%w+.-]*:') ~= nil
end

local function display_url(target)
  return (target:gsub('^mailto:', ''))
end

local function url_inline(target)
  return pandoc.Link({pandoc.Str(display_url(target))}, target)
end

local function rewrite_links(inline_mode)
  return {
    Link = function(el)
      if not is_external(el.target) then
        return nil
      end
      local text = stringify(el.content)
      local shown = display_url(el.target)
      if text == el.target or text == shown
          or text == shown:gsub('^https?://', ''):gsub('/$', '') then
        return url_inline(el.target)
      end
      if inline_mode then
        local out = el.content
        out:insert(pandoc.Str(' ('))
        out:insert(url_inline(el.target))
        out:insert(pandoc.Str(')'))
        return out
      end
      local out = el.content
      out:insert(pandoc.Note({pandoc.Plain({url_inline(el.target)})}))
      return out
    end,
  }
end

function Header(el)
  return el:walk(rewrite_links(true))
end

function Table(el)
  return el:walk(rewrite_links(true))
end

function Div(el)
  if not el.classes:includes('admonition') then
    return nil
  end
  local kind = el.classes[2] or 'note'
  local title = el.attributes.title or ''
  local blocks = pandoc.List({
    pandoc.RawBlock('typst', '#admonition(kind: ' .. typst_string(kind)
      .. ', title: ' .. typst_string(title) .. ')['),
  })
  blocks:extend(el.content)
  blocks:insert(pandoc.RawBlock('typst', ']'))
  return blocks
end

-- Three passes: the first substitutes missing glyphs, the second handles admonitions and the links in headings and
-- tables (inline URLs), and the third turns every remaining web link into
-- text plus a footnote. A link already rewritten to show its URL is left
-- as it is by the second pass.
return {
  { Str = fix_text, Code = fix_text },
  { Header = Header, Table = Table, Div = Div },
  rewrite_links(false),
}
