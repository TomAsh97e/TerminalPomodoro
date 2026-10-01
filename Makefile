CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
PREFIX ?= /usr/local

PomodoroT: main.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

test: PomodoroT
	./PomodoroT --help >/dev/null
	! ./PomodoroT --nauka 0 --przerwa 5 >/dev/null 2>&1

install: PomodoroT
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	install -m 755 PomodoroT $(DESTDIR)$(PREFIX)/bin/PomodoroT

clean:
	rm -f PomodoroT

.PHONY: test install clean
