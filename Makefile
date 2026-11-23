CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
BIN     := bin
PREFIX  ?= $(HOME)/.local
AUTOSTART := $(HOME)/.config/autostart

TOOLS := $(BIN)/midmove
TESTS := $(BIN)/midmove-test $(BIN)/drag-end-test $(BIN)/drag-stress

all: $(TOOLS)

$(BIN)/midmove: midmove/midmove.c
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -o $@ $< -lX11 -ldl

$(BIN)/midmove-test: midmove/tests/midmove_test.c
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -o $@ $< -lX11 -ldl

$(BIN)/drag-end-test: midmove/tests/drag_end_test.c
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -o $@ $< -lX11 -ldl

$(BIN)/drag-stress: midmove/tests/drag_stress.c
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -o $@ $< -lX11 -ldl

tests: $(TESTS)

test: all $(TESTS)
	@echo "== запускаю midmove и автотесты =="
	@./$(BIN)/midmove & pid=$$!; sleep 1; \
	  if ./$(BIN)/midmove-test; then rc=0; else rc=1; fi; \
	  kill $$pid 2>/dev/null; exit $$rc

install: all
	install -Dm755 $(BIN)/midmove $(PREFIX)/bin/midmove
	install -d $(AUTOSTART)
	sed 's|@EXEC@|$(PREFIX)/bin/midmove|' midmove/midmove.desktop.in > $(AUTOSTART)/midmove.desktop
	@echo "установлено: $(PREFIX)/bin/midmove и $(AUTOSTART)/midmove.desktop"

uninstall:
	rm -f $(PREFIX)/bin/midmove $(AUTOSTART)/midmove.desktop
	@echo "удалено"

clean:
	rm -rf $(BIN)

.PHONY: all tests test install uninstall clean
