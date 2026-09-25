APP = clinic-notes
PKGS = gtk4 sqlite3 libcurl json-glib-1.0 pangocairo libzip
CFLAGS += -std=c17 -Wall -Wextra -Wno-deprecated-declarations -O2 $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))

.PHONY: all run test clean install

all: $(APP)

$(APP): src/main.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

run: $(APP)
	./$(APP)

test: $(APP)
	./$(APP) --test-anonymizer

clean:
	rm -f $(APP)

install: $(APP)
	install -Dm755 $(APP) $(HOME)/.local/bin/$(APP)
	install -Dm644 clinic-notes.desktop $(HOME)/.local/share/applications/clinic-notes.desktop
