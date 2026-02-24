CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -Wvla -D_GNU_SOURCE -Iinclude

OBJDIR = obj
COMMON_OBJ = $(OBJDIR)/sim_utils.o
MANAGER_OBJ = $(OBJDIR)/manager.o $(OBJDIR)/config.o $(COMMON_OBJ)
OPERATOR_OBJ = $(OBJDIR)/operator.o $(COMMON_OBJ)
USER_OBJ = $(OBJDIR)/user.o $(COMMON_OBJ)
BINARIES = manager operator user

.PHONY: all clean rebuild

all: $(BINARIES)

manager: $(MANAGER_OBJ)
	$(CC) $(CFLAGS) -o $@ $(MANAGER_OBJ)

operator: $(OPERATOR_OBJ)
	$(CC) $(CFLAGS) -o $@ $(OPERATOR_OBJ)

user: $(USER_OBJ)
	$(CC) $(CFLAGS) -o $@ $(USER_OBJ)

$(OBJDIR)/%.o: src/%.c include/sim.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR) $(BINARIES)

rebuild: clean all
