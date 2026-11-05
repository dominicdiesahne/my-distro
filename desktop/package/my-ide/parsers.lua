-- Run by my-ide.mk while the ISO is built:
--   nvim --headless -c "luafile parsers.lua"
-- Installs the tree-sitter parsers (syntax highlighting, indenting, code
-- structure) for LazyVim's languages plus a few more. The parsers are
-- compiled with my-distro's cross-compiler (CC), so they run on my-distro.

local langs = vim.deepcopy(LazyVim.opts("nvim-treesitter").ensure_installed or {})
vim.list_extend(langs, { "cpp", "rust", "go", "java", "make", "cmake", "dockerfile", "css", "sql" })

local ts = require("nvim-treesitter")

local function missing_langs()
  local installed, missing = {}, {}
  for _, lang in ipairs(ts.get_installed()) do
    installed[lang] = true
  end
  for _, lang in ipairs(langs) do
    if not installed[lang] then
      table.insert(missing, lang)
    end
  end
  return missing
end

-- LazyVim starts installing its own list at startup too, at the same time;
-- when both work on the same language one of them fails. So try again for
-- whatever is still missing.
local missing = langs
for _ = 1, 3 do
  ts.install(missing, { summary = true }):wait(30 * 60 * 1000)
  vim.wait(2000)
  missing = missing_langs()
  if #missing == 0 then
    break
  end
end

if #missing > 0 then
  io.stderr:write("parsers missing: " .. table.concat(missing, ", ") .. "\n")
  vim.cmd("cquit 1")
end
io.stdout:write(("%d tree-sitter parsers installed\n"):format(#langs))
vim.cmd("qall!")
