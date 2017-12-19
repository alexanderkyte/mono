
##
# Parameters:
#  $(1): arch
#  $(2): host_triple
#
# Flags:
#  desktop_$(1)_CFLAGS
#  desktop_$(1)_CXXFLAGS
#  desktop_$(1)_LDFLAGS
define DesktopTemplate

#_desktop_$(1)_AC_VARS= \
	#mono_cv_uscore=yes \
	#ac_cv_func_sched_getaffinity=no \
	#ac_cv_func_sched_setaffinity=no

#_desktop_$(1)_CFLAGS= \
	#-fno-omit-frame-pointer -Wl,-z,now -Wl,-z,relro -Wl,-z,noexecstack -fstack-protector $(if $(filter $(IS_RELEASE),true),-O2,-O0 -ggdb3) \
	#-DMONODROID=1 \
	#$$(desktop_$(1)_CFLAGS)

#_desktop_$(1)_CXXFLAGS= \
	#-fno-omit-frame-pointer -Wl,-z,now -Wl,-z,relro -Wl,-z,noexecstack -fstack-protector $(if $(filter $(IS_RELEASE),true),-O2,-O0 -ggdb3) \
	#-DMONODROID=1 \
	#$$(desktop_$(1)_CXXFLAGS)

#_desktop_$(1)_LDFLAGS= \
	#-ldl -lm -lc \
	#-Wl,-dynamic-linker=/system/bin/linker \
	#$$(desktop_$(1)_LDFLAGS)

_desktop_$(1)_CONFIGURE_ENVIRONMENT = \
	CFLAGS="$$(_desktop_$(1)_CFLAGS)" \
	CXXFLAGS="$$(_desktop_$(1)_CXXFLAGS) " \
	LDFLAGS="$$(_desktop_$(1)_LDFLAGS)" \
	CC="cc"

_desktop_$(1)_CONFIGURE_FLAGS= \
	--host=$(2) \
	--cache-file=$$(TOP)/sdks/builds/desktop-$(1).config.cache \
	--prefix=$$(TOP)/sdks/out/desktop-$(1) \
	--disable-boehm \
	--disable-iconv \
	--disable-mcs-build \
	--disable-nls \
	--enable-dynamic-btls \
	--enable-maintainer-mode \
	--with-sigaltstack=yes \
	--with-tls=pthread \
	--without-ikvm-native

.PHONY: package-desktop-$(1)
package-desktop-$(1):
	$$(MAKE) -C $$(TOP)/sdks/builds/desktop-$(1)/mono install
	$$(MAKE) -C $$(TOP)/sdks/builds/desktop-$(1)/support install

.stamp-desktop-$(1)-configure: $$(TOP)/configure 
	mkdir -p $$(TOP)/sdks/builds/desktop-$(1)
	cd $$(TOP)/sdks/builds/desktop-$(1) && $$(TOP)/configure $$(_desktop_$(1)_AC_VARS) $$(_desktop_$(1)_CONFIGURE_ENVIRONMENT) $$(_desktop_$(1)_CONFIGURE_FLAGS)
	touch $$@

.PHONY: clean-desktop-$(1)
clean-desktop-$(1):
	rm -rf $$(TOP)/sdks/builds/toolchains/desktop-$(1) $$(TOP)/sdks/builds/desktop-$(1) $$(TOP)/sdks/builds/desktop-$(1).config.cache

TARGETS += desktop-$(1)

endef

$(eval $(call DesktopTemplate,x86_64,x86_64-apple-darwin17.2.0))
