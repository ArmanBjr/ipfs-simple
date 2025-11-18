.PHONY: help up down

help: ## Show available commands
	@echo 'Usage: make [target]'
	@awk 'BEGIN {FS = ":.*?## "} /^[a-zA-Z_-]+:.*?## / {printf "  %-15s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

up: ## Start services
	sudo docker-compose up --build

down: ## Stop services
	sudo docker-compose down
