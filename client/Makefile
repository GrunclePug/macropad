include config.mk
.DEFAULT_GOAL := all

.PHONY: all sweep build run test fmt lint clean install uninstall windows screenshots nativecheck

all: build

# cargo never deletes a stale build. Over TARGET_MAX GiB, drop this crate's
# builds (dependencies stay built); if that isn't enough, everything.
sweep:
	@size=$$(du -s --block-size=1G target 2>/dev/null | cut -f1); \
	if [ "$${size:-0}" -gt $(TARGET_MAX) ]; then \
		echo "target/ is $$size GiB, over $(TARGET_MAX): cleaning the workspace's builds"; \
		$(CARGO) clean --workspace -q; \
		size=$$(du -s --block-size=1G target 2>/dev/null | cut -f1); \
		if [ "$${size:-0}" -gt $(TARGET_MAX) ]; then \
			echo "target/ is still $$size GiB: cleaning everything"; \
			$(CARGO) clean -q; \
		fi; \
	fi

build: sweep
	$(CARGO) build --release --locked

run: sweep
	$(CARGO) run -- $(ARGS)

# A binary crate has no doc tests.
test: sweep
	$(CARGO) test --workspace --all-targets --no-fail-fast

fmt:
	$(CARGO) fmt --all

lint: sweep
	$(CARGO) clippy --workspace --all-targets --locked -- -D warnings

# The Windows exe (needs `rustup target add $(WINDOWS)` and MinGW), then a
# check that it loads only Windows system DLLs.
windows: sweep
	$(CARGO) build --release --locked --target $(WINDOWS)
	tools/dllcheck target/$(WINDOWS)/release/$(APP).exe

# Every tab, rendered against the pretend pad (lavapipe works).
screenshots: sweep
	$(CARGO) run --release --locked -- --screenshot $(SHOTS)

# Release builds for Linux and Windows in fresh target directories, with
# logging compiler wrappers: fails if any C or C++ was compiled.
nativecheck:
	tools/nativecheck target/nativecheck $(WINDOWS)

clean:
	$(CARGO) clean

install: build
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 target/release/$(APP) $(DESTDIR)$(PREFIX)/bin/$(APP)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(APP)
