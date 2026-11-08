################################################################################
#
# my-emoji-font: Google's Noto Color Emoji (OFL-1.1), ~11 MB, the emoji of
# Unicode 18 as colour pictures. Buildroot has no emoji font. fontconfig
# finds it in /usr/share/fonts and uses it for every emoji that DejaVu
# lacks. The download is checked against my-emoji-font.hash.
#
################################################################################

MY_EMOJI_FONT_VERSION = v2026-09-24-unicode18_0
MY_EMOJI_FONT_SITE = https://raw.githubusercontent.com/googlefonts/noto-emoji/$(MY_EMOJI_FONT_VERSION)/2D/fonts
MY_EMOJI_FONT_SOURCE = NotoColorEmoji.ttf
MY_EMOJI_FONT_EXTRA_DOWNLOADS = $(MY_EMOJI_FONT_SITE)/LICENSE
MY_EMOJI_FONT_LICENSE = OFL-1.1
MY_EMOJI_FONT_LICENSE_FILES = LICENSE

# Two single files, not an archive: just copy them.
define MY_EMOJI_FONT_EXTRACT_CMDS
	cp $(MY_EMOJI_FONT_DL_DIR)/NotoColorEmoji.ttf $(MY_EMOJI_FONT_DL_DIR)/LICENSE $(@D)/
endef

define MY_EMOJI_FONT_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/NotoColorEmoji.ttf \
		$(TARGET_DIR)/usr/share/fonts/noto/NotoColorEmoji.ttf
endef

$(eval $(generic-package))
