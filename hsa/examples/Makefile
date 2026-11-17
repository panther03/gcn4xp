# Builds (or runs, or cleans) every example: make [all|run|clean]
EXAMPLES = asm-kernel dpp_reduce ds_bpermute fp16_native fp16_storage \
           s_memrealtime s_memrealtime_inline

GOAL := $(or $(MAKECMDGOALS),all)

$(GOAL): $(EXAMPLES)

$(EXAMPLES):
	$(MAKE) -C $@ $(GOAL)

.PHONY: $(GOAL) $(EXAMPLES)
