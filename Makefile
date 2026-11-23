CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
BIN     := bin
PREFIX  ?= $(HOME)/.local
AUTOSTART := $(HOME)/.config/autostart

TOOLS := $(BIN)/midmove $(BIN)/headbat
TESTS := $(BIN)/midmove-test $(BIN)/drag-end-test $(BIN)/drag-stress $(BIN)/headbat-protocol-test

all: $(TOOLS)

$(BIN)/midmove: midmove/midmove.c
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -o $@ $< -lX11 -ldl

# headbat: GTK/appindicator грузятся через dlopen, заголовки не нужны.
$(BIN)/headbat: headbat/headbat.c headbat/protocol.c headbat/headbat.h
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -o $@ headbat/headbat.c headbat/protocol.c -ldl

$(BIN)/headbat-protocol-test: headbat/tests/protocol_test.c headbat/protocol.c headbat/headbat.h
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -o $@ headbat/tests/protocol_test.c headbat/protocol.c

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
	  kill $$pid 2>/dev/null; \
	  ./$(BIN)/headbat-protocol-test || rc=1; \
	  exit $$rc

install: all
	install -Dm755 $(BIN)/midmove $(PREFIX)/bin/midmove
	install -Dm755 $(BIN)/headbat $(PREFIX)/bin/headbat
	install -d $(AUTOSTART)
	sed 's|@EXEC@|$(PREFIX)/bin/midmove|' midmove/midmove.desktop.in > $(AUTOSTART)/midmove.desktop
	sed 's|@EXEC@|$(PREFIX)/bin/headbat|' headbat/headbat.desktop.in > $(AUTOSTART)/headbat.desktop
	install -Dm644 headbat/icons/headbat.svg $(HOME)/.local/share/icons/hicolor/scalable/apps/headbat.svg
	@echo "установлено: $(PREFIX)/bin/{midmove,headbat}, автозапуск GNOME и иконка"
	@echo "для доступа к донглу G435 без root установите udev-rule (нужен sudo):"
	@echo "  sudo install -Dm644 headbat/70-headbat.rules /etc/udev/rules.d/70-headbat.rules"
	@echo "  sudo udevadm control --reload && sudo udevadm trigger"

uninstall:
	rm -f $(PREFIX)/bin/midmove $(PREFIX)/bin/headbat
	rm -f $(AUTOSTART)/midmove.desktop $(AUTOSTART)/headbat.desktop
	rm -f $(HOME)/.local/share/icons/hicolor/scalable/apps/headbat.svg
	@echo "удалено (udev-rule в /etc удалите вручную, если ставили)"

clean:
	rm -rf $(BIN)

.PHONY: all tests test install uninstall clean
