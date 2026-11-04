CC ?= /home/opnzos/local/openxl/bin/clang
CFLAGS ?= -O2 -fzos-le-char-mode=ascii -D_EXT -D_XOPEN_SOURCE_EXTENDED=1 -I./include -Wall
LDFLAGS ?=

BINDIR = bin
SRCDIR = src
OBJDIR = obj

SRCS = $(SRCDIR)/main.c \
       $(SRCDIR)/xmit.c \
       $(SRCDIR)/receive.c \
       $(SRCDIR)/inspect.c \
       $(SRCDIR)/netdata.c \
       $(SRCDIR)/dynalloc.c \
       $(SRCDIR)/utils.c

OBJS = $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o, $(SRCS))

TARGET = $(BINDIR)/ussxmit

all: $(TARGET) symlinks

$(TARGET): $(OBJS) | $(BINDIR)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BINDIR):
	mkdir -p $(BINDIR)

$(OBJDIR):
	mkdir -p $(OBJDIR)

symlinks: $(TARGET)
	ln -sf ussxmit $(BINDIR)/xmit
	ln -sf ussxmit $(BINDIR)/receive

clean:
	rm -rf $(OBJDIR) $(BINDIR)

.PHONY: all clean symlinks
