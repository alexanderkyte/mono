include $(top_srcdir)/mk/common.mk

BENCHMARKDOTNET_PATH=BenchmarkDotNet

SUBMODULES_CONFIG_FILE = $(top_srcdir)/mono/tests/microbenchmarks/SUBMODULES.json
include $(top_srcdir)/scripts/submodules/versions.mk

$(eval $(call ValidateVersionTemplate,BenchmarkDotNet,BENCHMARKDOTNET))

# Bump the given submodule to the revision given by the REV make variable
# If COMMIT is 1, commit the change
bump-BenchmarkDotNet: __bump-version-BenchmarkDotNet

# Bump the given submodule to the branch given by the BRANCH/REMOTE_BRANCH make variables
# If COMMIT is 1, commit the change
bump-branch-BenchmarkDotNet: __bump-branch-BenchmarkDotNet

# Bump the given submodule to its current GIT version
# If COMMIT is 1, commit the change
bump-current-BenchmarkDotNet: __bump-current-version-BenchmarkDotNet

clean-local:
	$(RM) -r $(BENCH_PATH)

EXTRA_DIST=SUBMODULES.json

BenchmarkDotNet.stamp: Makefile 
	$(MAKE) reset-BenchmarkDotNet
	touch $@

BenchmarkDotNet/src/BenchmarkDotNet/bin/Release/net46/BenchmarkDotNet.dll: BenchmarkDotNet.stamp
	cd BenchmarkDotNet && chmod +x ./build.sh && ./build.sh --skiptests=true

all-local: test

check-local: test

top_builddir = ../../../
mcs_topdir = $(top_builddir)/mcs
RUNTIME = MONO_PATH=$(mcs_topdir)/class/lib/$(DEFAULT_PROFILE) $(top_builddir)/runtime/mono-wrapper
CSC = $(RUNTIME) $(mcs_topdir)/class/lib/$(DEFAULT_PROFILE)/mcs.exe

define BenchmarkDotNetTemplate
#$(eval SOURCE_FILE_$(1):=$(shell test -z $(3) && echo $(1) || echo "$(3)"))
#$(eval TEST_FILE_$(1):=$(shell test -z $(3) && echo $(1) || echo "$(3)"))

src/$(1).exe:: src/$(1).cs src/Microbenchmarks.cs Makefile BenchmarkDotNet.stamp
	$(CSC) -unsafe src/$(1).cs src/Microbenchmarks.cs -r:$(top_builddir)/mono/tests/microbenchmarks/BenchmarkDotNet/src/BenchmarkDotNet/bin/Release/net46/BenchmarkDotNet.dll 

build-$(1): src/$(1).exe

test-$(1):: build-$(1)
	MONO_EXECUTABLE=$(top_builddir)/mono/mini/mono-sgen MONO_PATH=$(top_builddir)/mono/tests/microbenchmarks/BenchmarkDotNet/src/BenchmarkDotNet/bin/Release/net46:$(mcs_topdir)/class/lib/net_4_x $(top_builddir)/runtime/mono-wrapper src/$(1).exe

test:: test-$(1)

build:: build-$(1)

endef

#$(eval $(call BenchmarkDotNetTemplate,BinaryTrees))
#$(eval $(call BenchmarkDotNetTemplate,NBody))
#$(eval $(call BenchmarkDotNetTemplate,Mandelbrot))
#$(eval $(call BenchmarkDotNetTemplate,RegexRedux))
#$(eval $(call BenchmarkDotNetTemplate,SpectralNorm))
#$(eval $(call BenchmarkDotNetTemplate,Fannkuchredux))
#$(eval $(call BenchmarkDotNetTemplate,Fasta))
#$(eval $(call BenchmarkDotNetTemplate,KNucleotide))
#$(eval $(call BenchmarkDotNetTemplate,RevComp))

#$(eval $(call BenchmarkDotNetTemplate,Template))
