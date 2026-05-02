BUILD_DIR      := build
WASM_BUILD_DIR := build-wasm
RUNTIME_DIR    := Runtime
SERVE_PORT     := 8000

.PHONY: help build build-debug install run clean \
        wasm serve \
        snapshot-city snapshot-glat-trace snapshot-filler \
        branches push-master status

help:
	@echo "Targets:"
	@echo "  build              Native Release build (Ninja)"
	@echo "  build-debug        Native Debug build"
	@echo "  install            Copy DEMO binary into $(RUNTIME_DIR)/"
	@echo "  run                Build, install, run from $(RUNTIME_DIR)/"
	@echo "  clean              Remove $(BUILD_DIR) and $(WASM_BUILD_DIR)"
	@echo ""
	@echo "  wasm               (Re)configure with emcmake + build wasm artifacts"
	@echo "  serve              Serve wasm build at http://localhost:$(SERVE_PORT)/DEMO.html"
	@echo ""
	@echo "  snapshot-city      Run --snapshot=city headless dump"
	@echo "  snapshot-glat-trace  Run --snapshot=glat-trace CSV recorder"
	@echo "  snapshot-filler    Run --snapshot=filler synthetic harness"
	@echo ""
	@echo "  branches           Show local + remote branch state vs origin/master"
	@echo "  status             git status + diverge count"
	@echo "  push-master        git push origin master (after confirming locally)"

build:
	cmake -S . -B $(BUILD_DIR) -G Ninja
	cmake --build $(BUILD_DIR)

build-debug:
	cmake -S . -B $(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(BUILD_DIR)

install: build
	cp $(BUILD_DIR)/DEMO/DEMO $(RUNTIME_DIR)/DEMO

run: install
	cd $(RUNTIME_DIR) && ./DEMO

clean:
	rm -rf $(BUILD_DIR) $(WASM_BUILD_DIR)

wasm:
	@command -v emcmake >/dev/null 2>&1 || { \
		echo "emcmake not on PATH — source your emsdk_env.sh (e.g. 'source /opt/homebrew/Cellar/emscripten/*/libexec/emsdk_env.sh') or 'brew install emscripten'"; \
		exit 1; \
	}
	emcmake cmake -S . -B $(WASM_BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build $(WASM_BUILD_DIR)

serve: wasm
	@echo "Open http://localhost:$(SERVE_PORT)/DEMO.html"
	cd $(WASM_BUILD_DIR)/DEMO && python3 -m http.server $(SERVE_PORT)

snapshot-city: install
	cd $(RUNTIME_DIR) && ./DEMO --snapshot=city

snapshot-glat-trace: install
	cd $(RUNTIME_DIR) && ./DEMO --snapshot=glat-trace

snapshot-filler: install
	cd $(RUNTIME_DIR) && ./DEMO --snapshot=filler

branches:
	@echo "=== local ==="
	@git for-each-ref --format='  %(refname:short)  [%(upstream:short)]  %(committerdate:short)  %(subject)' refs/heads/
	@echo ""
	@echo "=== remote (sorted by date) ==="
	@git for-each-ref --sort=committerdate --format='  %(refname:short)  %(committerdate:short)  %(subject)' refs/remotes/origin/
	@echo ""
	@echo "=== ahead of origin/master ==="
	@for b in $$(git for-each-ref --format='%(refname:short)' refs/heads/); do \
		ahead=$$(git rev-list --count origin/master..$$b 2>/dev/null); \
		if [ "$$ahead" != "0" ]; then echo "  $$b: +$$ahead"; fi; \
	done

status:
	@git status -sb
	@echo ""
	@cur=$$(git rev-parse --abbrev-ref HEAD); \
	upstream=$$(git rev-parse --abbrev-ref --symbolic-full-name @{u} 2>/dev/null); \
	if [ -n "$$upstream" ]; then \
		ahead=$$(git rev-list --count $$upstream..HEAD); \
		behind=$$(git rev-list --count HEAD..$$upstream); \
		echo "$$cur vs $$upstream: +$$ahead / -$$behind"; \
	fi

push-master:
	@if [ "$$(git rev-parse --abbrev-ref HEAD)" != "master" ]; then \
		echo "Not on master (currently on $$(git rev-parse --abbrev-ref HEAD)). Aborting."; \
		exit 1; \
	fi
	git push origin master
