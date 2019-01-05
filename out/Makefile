#---------------------------------------- 
# This Makefile compiles with USE_MPI only
#---------------------------------------- 
ifndef LG_RT_DIR
$(error LG_RT_DIR variable is not defined, aborting build)
endif

# Flags for directing the runtime makefile what to include
DEBUG           ?= 1		# Include debugging symbols
OUTPUT_LEVEL    ?= LEVEL_DEBUG	# Compile time logging level
USE_CUDA        ?= 0		# Include CUDA support (requires CUDA)
USE_GASNET      ?= 0		# Include GASNet support (requires GASNet)
USE_MPI         ?= 1            # Include MPI support
USE_HDF         ?= 0		# Include HDF5 support (requires HDF5)
ALT_MAPPERS     ?= 0		# Include alternative mappers (not recommended)

# Put the binary file name here
OUTFILE		?= main
# List all the application source files here
GEN_SRC		?= main.cc		# .cc files
GEN_GPU_SRC	?=				# .cu files

# You can modify these variables, some will be appended to by the runtime makefile
INC_FLAGS	?= 
CC_FLAGS	?= -DUSE_MPI
NVCC_FLAGS	?=
GASNET_FLAGS	?=
LD_FLAGS	?=

REALM_SRC = $(LG_RT_DIR)/realm/activemsg_impl_mpi.cc $(LG_RT_DIR)/realm/am_mpi.cc
include $(LG_RT_DIR)/runtime.mk

