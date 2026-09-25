CC = gcc
PKGS = sqlite3 libzip libpcre2-8 json-c
CFLAGS += -std=c17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS)) -lcomctl32 -lcomdlg32 -lwinhttp -lshell32 -lgdi32 -luser32 -ladvapi32

clinic-notes.exe: main.c privacy.c privacy.h
	$(CC) $(CFLAGS) -municode -mwindows main.c privacy.c -o $@ $(LDLIBS)

.PHONY: clean test
privacy-test.exe: privacy.c privacy_test.c privacy.h
	$(CC) $(CFLAGS) privacy.c privacy_test.c -o $@ $(shell pkg-config --libs libpcre2-8)

test: privacy-test.exe
	./privacy-test.exe

clean:
	rm -f clinic-notes.exe privacy-test.exe
