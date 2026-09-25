APP = clinic-notes
PKGS = libpcre2-8
CFLAGS += -std=c17 -Wall -Wextra $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))

.PHONY: all run test clean

all: $(APP)

$(APP): main.c privacy.c privacy.h
	$(CC) $(CFLAGS) main.c privacy.c -o $@ $(LDLIBS)

run: $(APP)
	./$(APP) --test-anonymizer

test: $(APP)
	./$(APP) --test-anonymizer

clean:
	rm -f $(APP)
