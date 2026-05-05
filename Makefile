CC ?= cc
PKG_CONFIG ?= pkg-config

APP := c-code
PROD_APP := dont_touch/c-code
SRC := \
	src/main.c \
	src/store.c \
	src/config.c \
	src/common.c \
	src/agent.c \
	src/provider_openrouter.c \
	src/tool_edit.c \
	src/tool_bash.c
OBJ := $(SRC:.c=.o)

CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
LDLIBS += $(shell $(PKG_CONFIG) --libs raylib libcurl sqlite3) -lpthread -lm
CFLAGS += $(shell $(PKG_CONFIG) --cflags raylib libcurl sqlite3)

.PHONY: all clean run prod install-agents

all: $(APP)

$(APP): $(OBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/*.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

run: $(APP)
	./$(APP)

prod: $(OBJ)
	mkdir -p dont_touch
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $(PROD_APP) $(OBJ) $(LDLIBS)

install-agents:
	mkdir -p .agents/agents .agents/providers
	cp -r templates/.agents/* .agents/

clean:
	rm -f $(APP) agentic-c $(OBJ)
