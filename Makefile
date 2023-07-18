# This Makefile defines a bunch of stuff used by the lower-level Makefiles
# all file paths are from their perspective!

CC = gcc

BIN = ../bin
BUILD = ../build
OBJ = $(BUILD)/obj

# this is the name of the lower level directory (e.g. THIS_DIR in the dev_handler folder is "dev_handler")
THIS_DIR = $(strip $(shell pwd | xargs basename -z))

COMPONENT_DIRS = dev_handler net_handler executor network_switch shm_wrapper runtime_util logger # list of runtime component directories
OBJ_SUBDIR = $(foreach dir,$(COMPONENT_DIRS),$(OBJ)/$(dir)) # list of subfolders in $(OBJ) for runtime object files
INCLUDE = $(foreach dir,$(COMPONENT_DIRS),-I../$(dir)) # list of folders to include in compilation commands

OBJS = $(patsubst %.c,$(OBJ)/$(THIS_DIR)/%.o,$(SRCS)) # make a list of all object files this executable is dependent on relative to this Makefile

.PHONY: all clean

# list of build folders
BUILD_DIR = $(OBJ) $(BIN) $(BUILD) $(OBJ_SUBDIR)

# general rule for compiling a list of source files to object files in the $(OBJ) directory
$(OBJ)/$(THIS_DIR)/%.o: %.c | $(OBJ_SUBDIR)
	$(CC) $(CFLAGS) -MMD -MP $(INCLUDE) -c $< -o $@

# general rule for making a build directory
$(BUILD_DIR):
	mkdir -p $@
