MP3DEC_MOD_DIR := $(USERMOD_DIR)

# Add all C files to SRC_USERMOD.
SRC_USERMOD += $(MP3DEC_MOD_DIR)/mp3dec.c

# Add our module directory to include paths
CFLAGS_USERMOD += -I$(MP3DEC_MOD_DIR)