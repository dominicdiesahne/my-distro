################################################################################
#
# my-search-model: the embedding model all-MiniLM-L6-v2 (Apache-2.0, by
# sentence-transformers) in GGUF format, 8-bit, ~25 MB, from Hugging Face.
# The download is checked against my-search-model.hash.
#
################################################################################

MY_SEARCH_MODEL_VERSION = 1.0
MY_SEARCH_MODEL_SOURCE = all-MiniLM-L6-v2-Q8_0.gguf
MY_SEARCH_MODEL_SITE = https://huggingface.co/second-state/All-MiniLM-L6-v2-Embedding-GGUF/resolve/main
MY_SEARCH_MODEL_LICENSE = Apache-2.0

# It's a single file, not an archive: just copy it.
define MY_SEARCH_MODEL_EXTRACT_CMDS
	cp $(MY_SEARCH_MODEL_DL_DIR)/$(MY_SEARCH_MODEL_SOURCE) $(@D)/
endef

define MY_SEARCH_MODEL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/$(MY_SEARCH_MODEL_SOURCE) \
		$(TARGET_DIR)/usr/share/my-search/$(MY_SEARCH_MODEL_SOURCE)
endef

$(eval $(generic-package))
