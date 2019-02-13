# -*- makefile -*-

BOOTSTRAP_PROFILE = build

BOOTSTRAP_MCS = MONO_PATH="$(topdir)/class/lib/$(BOOTSTRAP_PROFILE)$(PLATFORM_PATH_SEPARATOR)$$MONO_PATH" $(INTERNAL_CSC)
MCS = $(BOOTSTRAP_MCS)

profile-check:
	@:

DEFAULT_REFERENCES = mscorlib
PROFILE_MCS_FLAGS = \
	-d:NET_1_1 \
	-d:NET_2_0 \
	-d:NET_2_1 \
	-d:NET_3_5 \
	-d:NET_4_0 \
	-d:NET_4_5 \
	-d:MOBILE,MOBILE_LEGACY \
	-d:MONO \
	-d:MONOTOUCH \
	-d:DISABLE_REMOTING \
	-d:DISABLE_COM \
	-d:FEATURE_INTERCEPTABLE_THREADPOOL_CALLBACK \
	-nowarn:1699 \
	-nostdlib \
	$(PLATFORM_DEBUG_FLAGS) \
	$(MONOTOUCH_MCS_FLAGS)

API_BIN_PROFILE = build/monotouch
FRAMEWORK_VERSION = 2.1

# This is utility build only
NO_INSTALL = yes
AOT_FRIENDLY_PROFILE = yes
MOBILE_PROFILE = yes
NO_CONSOLE = yes

PROFILE_DISABLE_BTLS=1
MONO_FEATURE_APPLETLS=1
ONLY_APPLETLS=1
ENABLE_GSS=1