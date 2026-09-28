# Silver Remaster -- build with MinGW-w64 gcc (e.g. WinLibs) on PATH:
#   make          -> build/silver_remaster.exe
#   make run      -> build and start (optionally: make run ROOM=gno/screen1)
CC      = gcc
CFLAGS  = -O2 -Wall -Iinclude
LDLIBS  = -mwindows -luser32 -lgdi32 -lgdiplus -ldwmapi -lm
SRCS    = src/main.c src/character.c src/gltf.c src/json.c src/image.c
HEADERS = include/character.h include/gltf.h include/json.h include/image.h
EXE     = build/silver_remaster.exe
ROOM    =

.PHONY: all run clean

all: $(EXE)

$(EXE): $(SRCS) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

run: $(EXE)
	./$(EXE) $(ROOM)

clean:
	rm -f $(EXE)
