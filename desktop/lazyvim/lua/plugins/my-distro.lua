-- my-distro's changes to LazyVim. Add your own plugins in more files here.

-- The color theme picked in the Settings app (theme = ... in the settings
-- file): the "light" theme gets the light Tokyo Night, all others the dark.
local function distro_theme()
  local theme = "teal"
  for _, path in ipairs({ "/etc/my-distro/settings", vim.fn.expand("~/.config/my-distro/settings") }) do
    local f = io.open(path)
    if f then
      for line in f:lines() do
        local value = line:match("^%s*theme%s*=%s*(%S+)")
        if value then
          theme = value
        end
      end
      f:close()
    end
  end
  return theme
end

return {
  {
    "folke/tokyonight.nvim",
    opts = { style = distro_theme() == "light" and "day" or "night" },
  },

  -- Tree-sitter parsers can only be added with a C compiler, which my-distro
  -- doesn't have; the ones below were compiled when the ISO was built
  -- (desktop/package/my-ide/parsers.lua). Don't try to install more.
  {
    "nvim-treesitter/nvim-treesitter",
    opts = function(_, opts)
      if vim.fn.executable("cc") == 0 and vim.fn.executable("tree-sitter") == 0 then
        opts.ensure_installed = {}
      end
    end,
  },
}
