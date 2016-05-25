
with_mono_path = MONO_PATH="$(mcs_topdir)/class/lib/$(PROFILE)$(PLATFORM_PATH_SEPARATOR)$$MONO_PATH"
with_mono_path_monolite = MONO_PATH="$(mcs_topdir)/class/lib/monolite$(PLATFORM_PATH_SEPARATOR)$$MONO_PATH"

monolite_flag := $(depsdir)/use-monolite
use_monolite := $(wildcard $(monolite_flag))

MONOLITE_MCS = $(mcs_topdir)/class/lib/monolite/basic.exe

ifdef use_monolite
PROFILE_RUNTIME = $(with_mono_path_monolite) $(RUNTIME)
BOOTSTRAP_MCS = $(PROFILE_RUNTIME) $(RUNTIME_FLAGS) $(MONOLITE_MCS)
else
PROFILE_RUNTIME = $(EXTERNAL_RUNTIME)
BOOTSTRAP_MCS = $(EXTERNAL_MCS)
endif
