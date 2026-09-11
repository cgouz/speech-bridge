# Speech Bridge — orchestrator. Real work lives in scripts/build.sh.
# Every native build (cores + server) is CMake, built natively for `uname -sm`.

SHELL := /usr/bin/env bash
ROOT  := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
JOBS  ?=

.DEFAULT_GOAL := help

.PHONY: help
help: ## show this help
	@grep -hE '^[a-zA-Z_-]+:.*?## ' $(MAKEFILE_LIST) | \
	  awk 'BEGIN{FS=":.*?## "}{printf "  \033[36m%-16s\033[0m %s\n", $$1, $$2}'

.PHONY: cores
cores: ## build the four isolated core libs into lib/
	@scripts/build.sh --cores-only $(if $(JOBS),-j $(JOBS),)

.PHONY: web
web: ## build the Vue frontend into web/dist/ (needs node/npm)
	@cd web/frontend && npm ci && npm run build

.PHONY: app
app: ## build the C++ sb-server (needs cores in lib/)
	@scripts/build.sh --app-only $(if $(JOBS),-j $(JOBS),)

.PHONY: all build
all build: ## build cores + app
	@scripts/build.sh $(if $(JOBS),-j $(JOBS),)

.PHONY: check-symbols
check-symbols: ## assert every lib exports only sb_*
	@scripts/check-symbols.sh

.PHONY: doctor
doctor: ## verify toolchain + vendored trees
	@scripts/doctor.sh

.PHONY: test
test: ## unit tests only — no models, no native libs
	@cmake -S server -B server/build -DCMAKE_BUILD_TYPE=Release >/dev/null
	@cmake --build server/build -j $(if $(JOBS),$(JOBS),$$(nproc)) --target sb_tests
	@server/build/tests/sb_tests

.PHONY: test-e2e
test-e2e: ## end-to-end tests — needs models + cores + server, skips cleanly if absent
	@scripts/test-e2e.sh

.PHONY: check
check: ## build cores -> check-symbols -> smoke test -> build server -> unit tests
	@scripts/build.sh --cores-only $(if $(JOBS),-j $(JOBS),)
	@$(MAKE) app
	@$(MAKE) test

.PHONY: run
run: app ## run sb-server with the current environment
	@SB_LIB_DIR=$(ROOT)lib ./sb-server

.PHONY: fetch-models
fetch-models: ## download + sha256-verify model weights
	@scripts/fetch-models.sh

.PHONY: bench
bench: ## measure the latency budget
	@scripts/bench.sh

.PHONY: clean
clean: ## remove build outputs
	@rm -rf build lib server/build sb-server
	@echo "cleaned"
