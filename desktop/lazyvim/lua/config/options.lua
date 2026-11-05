-- Options are automatically loaded before lazy.nvim startup
-- Default options that are always set: https://github.com/LazyVim/LazyVim/blob/main/lua/lazyvim/config/options.lua
-- Add any additional options here

-- Clipboard: my-distro has no xclip/xsel, so copy to the desktop's clipboard
-- through the terminal (OSC 52; xterm is set up to allow it in ~/.Xresources).
-- Pasting uses Neovim's own register; paste text from other apps with
-- Shift+Insert or the middle mouse button.
if vim.fn.executable("xclip") == 0 and vim.fn.executable("xsel") == 0 then
  local osc52 = require("vim.ui.clipboard.osc52")
  local function paste()
    return { vim.fn.split(vim.fn.getreg(""), "\n"), vim.fn.getregtype("") }
  end
  vim.g.clipboard = {
    name = "OSC 52",
    copy = { ["+"] = osc52.copy("+"), ["*"] = osc52.copy("*") },
    paste = { ["+"] = paste, ["*"] = paste },
  }
end
