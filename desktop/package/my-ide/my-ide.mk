################################################################################
#
# my-ide: Neovim + LazyVim, ready to use without internet
#
# Buildroot has no Neovim, so we take the official prebuilt one (x86-64,
# glibc, like my-distro). The same binary also runs on the build machine,
# where it installs LazyVim's plugins (git clones) and the tree-sitter
# parsers. The parsers are C code; they are compiled with my-distro's
# cross-compiler, so my-distro itself needs no compiler.
#
# The config is desktop/lazyvim (becomes ~/.config/nvim of every user, via
# /etc/skel). desktop/lazyvim/lazy-lock.json pins the plugin versions.
#
# The plugins and parsers (~150 MB) are installed once, read-only, in
# /usr/share/my-ide. Each user's ~/.local/share/nvim/lazy and .../site are
# links to them, so a new user doesn't get a 150 MB copy. Neovim only reads
# them; ":Lazy update" would need write access and doesn't work.
#
################################################################################

MY_IDE_VERSION = 0.12.6
MY_IDE_SITE = https://github.com/neovim/neovim/releases/download/v$(MY_IDE_VERSION)
MY_IDE_SOURCE = nvim-linux-x86_64.tar.gz
MY_IDE_TREE_SITTER_VERSION = 0.27.1
MY_IDE_FONT_VERSION = 3.5.1
MY_IDE_EXTRA_DOWNLOADS = \
	https://github.com/tree-sitter/tree-sitter/releases/download/v$(MY_IDE_TREE_SITTER_VERSION)/tree-sitter-cli-linux-x64.zip \
	https://github.com/ryanoasis/nerd-fonts/releases/download/v$(MY_IDE_FONT_VERSION)/JetBrainsMono.tar.xz
MY_IDE_LICENSE = Apache-2.0 (Neovim, LazyVim), OFL-1.1 (JetBrains Mono)

MY_IDE_CONFIG = $(BR2_EXTERNAL_MY_DISTRO_PATH)/lazyvim
MY_IDE_HOME = $(@D)/home
MY_IDE_SKEL = $(TARGET_DIR)/etc/skel
MY_IDE_SHARED = $(TARGET_DIR)/usr/share/my-ide

# Neovim on the build machine: its own home folder, our tools first in PATH
# (tree-sitter CLI, and "cc" = my-distro's cross-compiler)
MY_IDE_NVIM = env -i HOME=$(MY_IDE_HOME) TERM=dumb \
	PATH=$(@D)/tools:$(@D)/bin:/usr/local/bin:/usr/bin:/bin \
	CC=$(@D)/tools/cc $(@D)/bin/nvim --headless

define MY_IDE_BUILD_CMDS
	rm -rf $(MY_IDE_HOME) $(@D)/tools
	mkdir -p $(MY_IDE_HOME)/.config $(@D)/tools
	cp -r $(MY_IDE_CONFIG) $(MY_IDE_HOME)/.config/nvim
	cd $(@D)/tools && unzip -o -q $(MY_IDE_DL_DIR)/tree-sitter-cli-linux-x64.zip && \
		chmod 755 tree-sitter
	# a script, not a symlink: Buildroot's compiler wrapper finds the real
	# compiler relative to its own path
	printf '#!/bin/sh\nexec %s "$$@"\n' "$(TARGET_CC)" > $(@D)/tools/cc
	chmod 755 $(@D)/tools/cc
	# 1. LazyVim and its plugins, at the versions in lazy-lock.json if there is one
	if [ -f $(MY_IDE_CONFIG)/lazy-lock.json ]; then \
		$(MY_IDE_NVIM) "+Lazy! restore" +qa; \
	else \
		$(MY_IDE_NVIM) "+Lazy! sync" +qa; \
	fi
	# 2. tree-sitter parsers
	$(MY_IDE_NVIM) -c "luafile $(MY_IDE_PKGDIR)/parsers.lua"
	# 3. nvim-treesitter links each language's queries (what to color) with
	#    an absolute path into this build folder: make the links relative,
	#    so they still work in /usr/share/my-ide
	find $(MY_IDE_HOME) -type l -lname '$(MY_IDE_HOME)/*' | while read -r link; do \
		ln -sfn "$$(realpath -m --relative-to="$$(dirname "$$link")" "$$(readlink "$$link")")" "$$link"; \
	done
	! find $(MY_IDE_HOME) -type l -lname '/*' | grep .
endef

define MY_IDE_INSTALL_TARGET_CMDS
	# Neovim itself
	mkdir -p $(TARGET_DIR)/usr/lib/nvim $(TARGET_DIR)/usr/bin
	cp -a $(@D)/bin $(@D)/lib $(@D)/share $(TARGET_DIR)/usr/lib/nvim/
	ln -sf ../lib/nvim/bin/nvim $(TARGET_DIR)/usr/bin/nvim
	# Plugins and parsers, shared by all users (see the top of this file)
	rm -rf $(MY_IDE_SHARED)
	mkdir -p $(MY_IDE_SHARED)
	cp -a $(MY_IDE_HOME)/.local/share/nvim/lazy $(MY_IDE_HOME)/.local/share/nvim/site \
		$(MY_IDE_SHARED)/
	# Every user's own part, in /etc/skel: the config, Mason's folder (it
	# installs language servers there) and the links to the shared part.
	# (Up to version 0.6.1 all of it was in /home/dominic: remove that copy.)
	rm -rf $(TARGET_DIR)/home/dominic/.config/nvim $(TARGET_DIR)/home/dominic/.local/share/nvim
	rm -rf $(MY_IDE_SKEL)/.config/nvim $(MY_IDE_SKEL)/.local/share/nvim
	mkdir -p $(MY_IDE_SKEL)/.config $(MY_IDE_SKEL)/.local/share/nvim
	cp -a $(MY_IDE_HOME)/.config/nvim $(MY_IDE_SKEL)/.config/nvim
	cp -a $(MY_IDE_HOME)/.local/share/nvim/mason $(MY_IDE_SKEL)/.local/share/nvim/
	ln -s /usr/share/my-ide/lazy $(MY_IDE_SKEL)/.local/share/nvim/lazy
	ln -s /usr/share/my-ide/site $(MY_IDE_SKEL)/.local/share/nvim/site
	# The font with the icons LazyVim shows (a "Nerd Font")
	mkdir -p $(TARGET_DIR)/usr/share/fonts/nerd
	tar -xJf $(MY_IDE_DL_DIR)/JetBrainsMono.tar.xz -C $(TARGET_DIR)/usr/share/fonts/nerd \
		JetBrainsMonoNerdFontMono-Regular.ttf JetBrainsMonoNerdFontMono-Bold.ttf \
		JetBrainsMonoNerdFontMono-Italic.ttf JetBrainsMonoNerdFontMono-BoldItalic.ttf
endef

$(eval $(generic-package))
