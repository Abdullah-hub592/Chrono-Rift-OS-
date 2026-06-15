

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -pthread

# Each folder carries its own headers — no shared include/ dir needed
LDFLAGS      := -pthread -lrt
# LIBS line — uncomment for SFML (GUI choice):
LDFLAGS_SFML := -pthread -lrt -lsfml-graphics -lsfml-window -lsfml-system

BIN := bin

.PHONY: all clean run

all: $(BIN) \
     $(BIN)/chrono_rift \
     $(BIN)/arbiter \
     $(BIN)/hip \
     $(BIN)/asp \
     $(BIN)/renderer \
     $(BIN)/multiplayer

$(BIN):
	mkdir -p $(BIN)

# ── Main launcher (lives in arbiter/) ────────────────────────────────────────
$(BIN)/chrono_rift: arbiter/main.cpp arbiter/shared_types.h arbiter/shm_utils.h
	$(CXX) $(CXXFLAGS) -I./arbiter -o $@ $< $(LDFLAGS)

# ── Arbiter ───────────────────────────────────────────────────────────────────
$(BIN)/arbiter: arbiter/arbiter.cpp \
                arbiter/shared_types.h arbiter/shm_utils.h \
                arbiter/inventory.h arbiter/game_log.h arbiter/signals_def.h
	$(CXX) $(CXXFLAGS) -I./arbiter -o $@ $< $(LDFLAGS)

# ── HIP ───────────────────────────────────────────────────────────────────────
$(BIN)/hip: hip/hip.cpp \
            hip/shared_types.h hip/shm_utils.h \
            hip/inventory.h hip/game_log.h hip/signals_def.h
	$(CXX) $(CXXFLAGS) -I./hip -o $@ $< $(LDFLAGS)

# ── ASP ───────────────────────────────────────────────────────────────────────
$(BIN)/asp: asp/asp.cpp \
            asp/shared_types.h asp/shm_utils.h \
            asp/inventory.h asp/game_log.h asp/signals_def.h
	$(CXX) $(CXXFLAGS) -I./asp -o $@ $< $(LDFLAGS)

# ── Renderer (helper in arbiter/) ────────────────────────────────────────────
$(BIN)/renderer: arbiter/renderer.cpp \
                 arbiter/shared_types.h arbiter/shm_utils.h \
                 arbiter/inventory.h arbiter/game_log.h
	$(CXX) $(CXXFLAGS) -I./arbiter -o $@ $< $(LDFLAGS_SFML)

# ── Multiplayer (helper in arbiter/) ─────────────────────────────────────────
$(BIN)/multiplayer: arbiter/multiplayer.cpp \
                    arbiter/shared_types.h arbiter/shm_utils.h \
                    arbiter/inventory.h arbiter/game_log.h arbiter/signals_def.h
	$(CXX) $(CXXFLAGS) -I./arbiter -o $@ $< $(LDFLAGS)

# ── Run ───────────────────────────────────────────────────────────────────────
run: all
	cd $(BIN) && ./chrono_rift

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BIN)
	ipcrm -M 0x43524946 2>/dev/null || true
