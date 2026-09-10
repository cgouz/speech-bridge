# Speech Bridge — orchestrator. Real work lives in scripts/build.sh.
# cgo forbids cross-compilation: every target builds natively for `uname -sm`.

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

.PHONY: app
app: ## go build sb-server (needs cores in lib/)
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
	@go test ./app/... ./web/...

.PHONY: test-e2e
test-e2e: ## end-to-end tests — needs models, skips cleanly if absent
	@SB_E2E=1 go test -tags e2e ./app/...

.PHONY: check
check: ## build cores -> check-symbols -> smoke test -> go vet -> go test
	@scripts/build.sh --cores-only $(if $(JOBS),-j $(JOBS),)
	@scripts/check-symbols.sh
	@if [[ -x build/smoke/sb_smoke ]]; then build/smoke/sb_smoke; else echo "smoke test: milestone 2"; fi
	@go vet ./app/... ./web/... && go test ./app/... ./web/...

.PHONY: run
run: app ## run sb-server with the current environment
	@SB_LIB_DIR=$(ROOT)lib ./app/sb-server

.PHONY: fetch-models
fetch-models: ## download + sha256-verify model weights
	@scripts/fetch-models.sh

.PHONY: bench
bench: ## measure the latency budget
	@scripts/bench.sh

.PHONY: clean
clean: ## remove build outputs
	@rm -rf build lib app/sb-server
	@echo "cleaned"
