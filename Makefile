CC ?= cc
CFLAGS ?= -O3 -Wall -Wextra -std=c99 -DVGR3_FEAT_EXT=1

all: vgm2vgr3

# VGR3 encoder (see new.md, vgr3_format.h). Self-verifies with vgr3_play.c.
vgm2vgr3: vgm2vgr3.c vgr3_play.c vgr3_format.h vgr3_play.h vgm_format.h
	$(CC) $(CFLAGS) -o $@ vgm2vgr3.c vgr3_play.c

SAMPLES_DIR ?= samples
VGR3_DIR ?= vgr3
ROUNDTRIP_OUT := .roundtrip

# Encode every sample to $(VGR3_DIR)/<name>.vgr3. vgm2vgr3 is
# self-verifying, so a file is only written when its decode matches the
# source frame by frame (and --loop replays two full passes).
samples: vgm2vgr3
	@mkdir -p $(VGR3_DIR)
	@pass=0; fail=0; total=0; \
	for f in $$(find $(SAMPLES_DIR) -type f -iname '*.vgm' | sort); do \
		base=$$(basename "$$f"); \
		out="$(VGR3_DIR)/$${base%.*}.vgr3"; \
		log="$(VGR3_DIR)/$${base%.*}.log"; \
		if ./vgm2vgr3 --loop "$$f" "$$out" > "$$log" 2>&1; then \
			rm -f "$$log"; \
			pass=$$((pass+1)); sz=$$(wc -c < "$$out"); total=$$((total+sz)); \
			echo "OK    $$out  ($$sz bytes)"; \
		else \
			fail=$$((fail+1)); echo "FAIL  $$base  (see $$log)"; \
		fi; \
	done; \
	echo "---"; \
	echo "samples: $$pass built, $$fail failed, $$total bytes"; \
	[ $$fail -eq 0 ]

# Encodes every sample to VGR3; vgm2vgr3 exits non-zero (and writes
# nothing) unless the decoded file matches the source frame by frame.
roundtrip: vgm2vgr3
	@mkdir -p $(ROUNDTRIP_OUT)
	@pass=0; fail=0; total=0; \
	for f in $$(find $(SAMPLES_DIR) -type f \( -iname '*.vgm' \) | sort); do \
		base=$$(basename "$$f"); \
		vgr="$(ROUNDTRIP_OUT)/$$base.vgr"; \
		log="$(ROUNDTRIP_OUT)/$$base.log"; \
		if ./vgm2vgr3 "$$f" "$$vgr" > "$$log" 2>&1; then \
			pass=$$((pass+1)); sz=$$(wc -c < "$$vgr"); total=$$((total+sz)); \
			echo "PASS  $$base  ($$sz bytes)"; \
		else \
			fail=$$((fail+1)); echo "FAIL  $$base  (see $$log)"; \
		fi; \
	done; \
	echo "---"; \
	echo "roundtrip: $$pass passed, $$fail failed, $$total bytes"; \
	[ $$fail -eq 0 ]

# Same as roundtrip but with --loop, so sources with no loop point get
# a synthetic JUMP back to frame 0. The encoder's self-check already
# replays two full passes whenever loopFrame is set, so a clean run here
# verifies the loop-around state (including the frame-0 reset) too.
roundtrip-loop: vgm2vgr3
	@mkdir -p $(ROUNDTRIP_OUT)
	@pass=0; fail=0; \
	for f in $$(find $(SAMPLES_DIR) -type f \( -iname '*.vgm' \) | sort); do \
		base=$$(basename "$$f"); \
		vgr="$(ROUNDTRIP_OUT)/loop-$$base.vgr"; \
		log="$(ROUNDTRIP_OUT)/loop-$$base.log"; \
		if ./vgm2vgr3 --loop "$$f" "$$vgr" > "$$log" 2>&1; then \
			pass=$$((pass+1)); echo "PASS  $$base"; \
		else \
			fail=$$((fail+1)); echo "FAIL  $$base  (see $$log)"; \
		fi; \
	done; \
	echo "---"; \
	echo "roundtrip-loop: $$pass passed, $$fail failed"; \
	[ $$fail -eq 0 ]

clean:
	rm -f vgm2vgr3 *.o
	rm -rf $(ROUNDTRIP_OUT)

.PHONY: all clean samples roundtrip roundtrip-loop fuzz-build fuzz-seeds

# Mixer host test (-DVGR3_MIXER): overlays a short sfx on a music track and
# checks the merged output against two independent players.
test/test_mixer: test/test_mixer.c vgr3_play.c vgr3_play.h vgr3_format.h
	$(CC) $(CFLAGS) -I. -DVGR3_MIXER -o $@ test/test_mixer.c vgr3_play.c

test/test_fade: test/test_fade.c vgr3_play.c vgr3_play.h vgr3_format.h
	$(CC) $(CFLAGS) -I. -DVGR3_MIXER -DVGR3_FADE -o $@ test/test_fade.c vgr3_play.c

mixertest: vgm2vgr3 test/test_mixer test/test_fade
	@mkdir -p $(ROUNDTRIP_OUT)
	./vgm2vgr3 --loop samples/sn76489/nightmarket.vgm $(ROUNDTRIP_OUT)/mx-music.vgr3 >/dev/null
	./vgm2vgr3 samples/sn76489/8Jump.vgm $(ROUNDTRIP_OUT)/mx-sfx.vgr3 >/dev/null
	./test/test_mixer $(ROUNDTRIP_OUT)/mx-music.vgr3 $(ROUNDTRIP_OUT)/mx-sfx.vgr3
	./vgm2vgr3 --loop samples/nes/eiffel-nes.vgm $(ROUNDTRIP_OUT)/mx-nmusic.vgr3 >/dev/null
	./vgm2vgr3 samples/nes/famitune-nes.vgm $(ROUNDTRIP_OUT)/mx-nsfx.vgr3 >/dev/null
	./test/test_mixer $(ROUNDTRIP_OUT)/mx-nmusic.vgr3 $(ROUNDTRIP_OUT)/mx-nsfx.vgr3
	./test/test_fade $(ROUNDTRIP_OUT)/mx-music.vgr3 $(ROUNDTRIP_OUT)/mx-sfx.vgr3 sn 2 5 8 10
	./test/test_fade $(ROUNDTRIP_OUT)/mx-nmusic.vgr3 $(ROUNDTRIP_OUT)/mx-nsfx.vgr3 low 0 4 12

# NES mixer demo (nes/mixdemo.c): stages the sources and two .vgr3 files in
# nes/build/, then builds the ROM with the 8bitworkshop CLI if BWS points at
# a checkout (make nes-demo BWS=~/PuzzlingPlans/8bitworkshop).
nes-demo: vgm2vgr3
	@mkdir -p nes/build
	./vgm2vgr3 --loop samples/nes/eiffel-nes.vgm nes/build/eiffel.vgr3 >/dev/null
	./vgm2vgr3 samples/nes/famitune-nes.vgm nes/build/sfx.vgr3 >/dev/null
	cp nes/mixdemo.c vgr3_play.c vgr3_play.h vgr3_format.h nes/build/
	@if [ -n "$(BWS)" ]; then \
		cd $(BWS) && node gen/tools/8bws.js build -p nes $(CURDIR)/nes/build/mixdemo.c -o $(CURDIR)/nes/build/mixdemo.nes; \
	else echo "staged nes/build; build with: node gen/tools/8bws.js build -p nes nes/build/mixdemo.c -o mixdemo.nes"; fi

# ---- Fuzzing (needs afl++; see fuzz/README.md) ----
# Encoder (untrusted VGM in) and decoder (untrusted VGR3 in), both with
# ASan+UBSan. The decoder is also built with the mixer.
AFLCC ?= afl-clang-fast
FUZZFLAGS = -O1 -g -std=c99 -DVGR3_FEAT_EXT=1 -fsanitize=address,undefined -fno-sanitize-recover=undefined

fuzz-build:
	@mkdir -p fuzz/build
	AFL_USE_ASAN=1 $(AFLCC) $(FUZZFLAGS) -o fuzz/build/enc vgm2vgr3.c vgr3_play.c
	AFL_USE_ASAN=1 $(AFLCC) $(FUZZFLAGS) -o fuzz/build/dec fuzz/dec.c vgr3_play.c
	AFL_USE_ASAN=1 $(AFLCC) $(FUZZFLAGS) -DVGR3_MIXER -o fuzz/build/dec-mixer fuzz/dec.c vgr3_play.c

# Seeds: every sample VGM, and the encoder's own VGR3 output for the decoder.
fuzz-seeds: vgm2vgr3
	@mkdir -p fuzz/in-vgm fuzz/in-vgr3
	find $(SAMPLES_DIR) -type f -iname '*.vgm' -exec cp {} fuzz/in-vgm/ \;
	@for f in fuzz/in-vgm/*.vgm; do ./vgm2vgr3 --loop "$$f" "fuzz/in-vgr3/$$(basename $$f .vgm).vgr3" >/dev/null 2>&1 || true; done

fuzz-run: fuzz-build
	AFL_AUTORESUME=1 AFL_CRASH_EXITCODE=3 afl-fuzz -M m -i fuzz/in-vgm -o fuzz/out-enc -x fuzz/vgm.dict -m none -t 3000 -- fuzz/build/enc --loop --layout 0 --window 64 --no-cross --far-penalty 2 --dict 16 @@ /dev/null
	#AFL_AUTORESUME=1 AFL_CRASH_EXITCODE=3 afl-fuzz -M m -i fuzz/in-vgm -o fuzz/out-enc -x fuzz/vgm.dict -m none -t 3000 -- fuzz/build/enc --loop @@ /dev/null
