# compiler
CC=gcc

DXDIR = C:\bin\DirectX8SDK

DEFINES = 
DDEFINES = -D_DJ_DEBUG_ -D_DJ_DEBUG_OUT_=debug.txt

CFLAGS = -c -O3 -Wall -fmessage-length=0 $(DEFINES)
CFLAGS_D = -c -O0 -g3 -Wall -fmessage-length=0 $(DDEFINES)

INCDIR = -I../djutils -I$(DXDIR)/include

OBJ_DIR = obj
ROBJ_DIR = $(OBJ_DIR)/Release
DOBJ_DIR = $(OBJ_DIR)/Debug

LIB_DIR = lib

OBJS=	djmm_utils.o \
	dx_draw.o \
	dx_input.o \
	mid_player.o \
	mus_player.o \
	pcm_player.o 

ROBJS = $(OBJS:%.o=$(ROBJ_DIR)/%.o)
DOBJS = $(OBJS:%.o=$(DOBJ_DIR)/%.o)

# targets
LIB = $(LIB_DIR)/libdjmm.a
LIB_D = $(LIB_DIR)/libdjmmd.a

default: release

release: $(LIB)

debug: $(LIB_D)

$(LIB): $(ROBJS)
	mkdir -p $(LIB_DIR)
	ar rvs $@ $^

$(LIB_D): $(DOBJS)
	mkdir -p $(LIB_DIR)
	ar rvs $@ $^

$(ROBJ_DIR)/%.o: %.c %.h
	$(CC) -o $@ $(CFLAGS) $(INCDIR) $<

$(ROBJ_DIR)/%.o: %.c
	$(CC) -o $@ $(CFLAGS) $(INCDIR) $<

$(DOBJ_DIR)/%.o: %.c %.h
	$(CC) -o $@ $(CFLAGS_D) $(INCDIR) $<

$(DOBJ_DIR)/%.o: %.c
	$(CC) -o $@ $(CFLAGS_D) $(INCDIR) $<

$(ROBJS): | $(ROBJ_DIR)

$(DOBJS): | $(DOBJ_DIR)

$(ROBJ_DIR): | $(OBJ_DIR)
	mkdir -p $(ROBJ_DIR)

$(DOBJ_DIR): | $(OBJ_DIR)
	mkdir -p $(DOBJ_DIR)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

clean:
	rm -f $(DOBJS)
	rm -f $(ROBJS)

clobber: clean
	rm -f $(LIB) $(LIB_D)

