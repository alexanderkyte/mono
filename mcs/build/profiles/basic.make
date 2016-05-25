# -*- makefile -*-

include $(topdir)/build/monolite_or_extern.make

MCS = $(with_mono_path) $(INTERNAL_GMCS)

PROFILE_MCS_FLAGS = -d:NET_4_0 -d:NET_4_5 -d:MONO -d:BOOTSTRAP_BASIC -nowarn:1699 -d:DISABLE_CAS_USE -lib:$(mcs_topdir)/class/lib/$(PROFILE)
NO_SIGN_ASSEMBLY = yes
NO_TEST = yes
NO_INSTALL = yes
FRAMEWORK_VERSION = 4.0

# Compiler all using same bootstrap compiler
LIBRARY_COMPILE = $(BOOT_COMPILE)

# Verbose basic only
# V = 1

#
# Copy from rules.make because I don't know how to unset MCS_FLAGS
#
USE_MCS_FLAGS = /codepage:$(CODEPAGE) $(LOCAL_MCS_FLAGS) $(PLATFORM_MCS_FLAGS) $(PROFILE_MCS_FLAGS)

.PHONY: profile-check do-profile-check
profile-check:
	@:

ifeq (.,$(thisdir))
all-recursive: do-profile-check
all-local: post-profile-cleanup
clean-local: clean-profile
endif

clean-profile:
	-rm -f $(PROFILE_EXE) $(PROFILE_OUT) $(monolite_flag)

post-profile-cleanup:
	@rm -f $(monolite_flag)

PROFILE_EXE = $(depsdir)/basic-profile-check.exe
PROFILE_OUT = $(PROFILE_EXE:.exe=.out)

MAKE_Q=$(if $(V),,-s)

do-profile-check: $(depsdir)/.stamp
	@ok=:; \
	rm -f $(PROFILE_EXE) $(PROFILE_OUT); \
	$(MAKE) $(MAKE_Q) $(PROFILE_OUT) || ok=false; \
	if $$ok; then rm -f $(PROFILE_EXE) $(PROFILE_OUT); else \
	    if test -f $(MONOLITE_MCS); then \
		$(MAKE) -s do-profile-check-monolite ; \
	    else \
		echo "*** The compiler '$(BOOTSTRAP_MCS)' doesn't appear to be usable." 1>&2; \
                echo "*** You need Mono version 3.8 or better installed to build MCS" 1>&2 ; \
                echo "*** Check mono README for information on how to bootstrap a Mono installation." 1>&2 ; \
		echo "*** The version of '$(BOOTSTRAP_MCS)' is: `$(BOOTSTRAP_MCS) --version`." 1>&2 ; \
	        exit 1; fi; fi


ifdef use_monolite

do-profile-check-monolite:
	echo "*** The contents of your 'monolite' directory may be out-of-date" 1>&2
	echo "*** You may want to try 'make get-monolite-latest'" 1>&2
	rm -f $(monolite_flag)
	exit 1

else

do-profile-check-monolite: $(depsdir)/.stamp
	echo "*** The compiler '$(BOOTSTRAP_MCS)' doesn't appear to be usable." 1>&2
	echo "*** Trying the 'monolite' directory." 1>&2
	echo dummy > $(monolite_flag)
	$(MAKE) do-profile-check

endif

$(PROFILE_EXE): $(topdir)/build/common/basic-profile-check.cs
	$(BOOTSTRAP_MCS) /warn:0 /out:$@ $<
	echo -n "Bootstrap compiler: " 1>&2
	$(BOOTSTRAP_MCS) --version 1>&2

$(PROFILE_OUT): $(PROFILE_EXE)
	$(PROFILE_RUNTIME) $< > $@ 2>&1
