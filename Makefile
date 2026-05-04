# Quick configurations
ROOT := $(PWD)
OS_VER ?= 16.0
LLVM_ARCH := AArch64
APPLE_ARCH := arm64
TARGET_TRIPLE := $(APPLE_ARCH)-apple-ios$(OS_VER)
SWIFT_BRANCH ?= swift-6.3.1-RELEASE
SWIFT_SOURCE_DIR ?= swift
SWIFT_TOOLCHAIN_ZIP := SwiftToolchain.zip
SWIFT_TOOLCHAIN_ROOT ?= SwiftToolchain-iphoneos

# Helper function
define log_info
	@echo "\033[32m\033[1m[*] \033[0m\033[32m$(1)\033[0m"
endef

# Main Target
all: LLVM.xcframework

# Fetch & Build Swift and LLVM for iOS
swift:
	$(call log_info,fetching swift sources ($(SWIFT_BRANCH)))
	SWIFT_BRANCH="$(SWIFT_BRANCH)" SWIFT_SOURCE_DIR="$(SWIFT_SOURCE_DIR)" Scripts/build-swift-toolchain.sh fetch
	$(call log_info,bypassing lld darwin incompatibility)
	perl -i -0pe 's|(// Swift LLVM fork downstream change start\n)(.*?)(// Swift LLVM fork downstream change end\n)|$$1/* NYXIAN: apple lies, lld works fine for MachO\n$$2*/\n$$3|s' \
	llvm-project/lld/MachO/InputFiles.cpp
	$(call log_info,patching swift's darwin toolchain not creating compiler jobs)
	cp Patches/DarwinToolChains.cpp swift/lib/Driver/DarwinToolChains.cpp

SwiftToolchain-iphoneos: swift
	$(call log_info,building iOS-native swift toolchain ($(SWIFT_BRANCH)))
	SWIFT_BRANCH="$(SWIFT_BRANCH)" SWIFT_SOURCE_DIR="$(SWIFT_SOURCE_DIR)" Scripts/build-swift-toolchain.sh build

$(SWIFT_TOOLCHAIN_ZIP): SwiftToolchain-iphoneos
	$(call log_info,packaging iOS-native swift toolchain)
	Scripts/build-swift-toolchain.sh package

swift-toolchain: $(SWIFT_TOOLCHAIN_ZIP)

install-nyxian-swift-toolchain: $(SWIFT_TOOLCHAIN_ZIP)
	$(call log_info,installing swift toolchain ($(SWIFT_BRANCH)) into Nyxian Shared resources)
	Scripts/build-swift-toolchain.sh install-nyxian

verify-swift-toolchain:
	Scripts/build-swift-toolchain.sh verify-host

# Bundle
SDK = $(shell xcrun --sdk iphoneos --show-sdk-path)
SWIFT_STATIC_LIBS = $(wildcard $(SWIFT_TOOLCHAIN_ROOT)/lib/libswift*.a) \
                    $(wildcard $(SWIFT_TOOLCHAIN_ROOT)/lib/lib_CompilerRegexParser.a) \
                    $(wildcard $(SWIFT_TOOLCHAIN_ROOT)/lib/libclang*.a) \
                    $(wildcard $(SWIFT_TOOLCHAIN_ROOT)/lib/liblld*.a) \
                    $(wildcard $(SWIFT_TOOLCHAIN_ROOT)/lib/libLLVM*.a) \
                    $(wildcard $(ROOT)/build/LLVMClangSwift_iphoneos/cmark-iphoneos-arm64/src/libcmark-gfm.a) \
                    $(wildcard $(ROOT)/build/LLVMClangSwift_iphoneos/cmark-iphoneos-arm64/extensions/libcmark-gfm-extensions.a)
SWIFT_HOST_COMPILER_DYLIBS = $(wildcard $(SWIFT_TOOLCHAIN_ROOT)/lib/swift/host/compiler/lib_Compiler*.dylib)

LLVM.xcframework: swift-toolchain
LLVM.xcframework:
	$(call log_info,bundling CoreCompilerSupportLibs)
	-rm -rf CoreCompilerSupportLibs
	mkdir CoreCompilerSupportLibs
	cp $(SWIFT_HOST_COMPILER_DYLIBS) CoreCompilerSupportLibs/
	$(call log_info,bundling LLVM xcframework headers)
	-rm -rf Headers
	mkdir Headers
	cp -r SwiftToolchain-iphoneos/include/* Headers/
	cp -r llvm-project/lld/include/* Headers/
	cp -r llvm-project/clang/include/* Headers/
	cp -r llvm-project/llvm/include/* Headers/
	cp -r build/LLVMClangSwift_iphoneos/llvm-iphoneos-arm64/tools/clang/include/* Headers/
	rm -rf Headers/swift/Bridging
	cp -r swift/include/* Headers/
	cp -r swift/stdlib/public/SwiftShims/* Headers/
	$(call log_info,create llvm.a)
	-rm -rf llvm.a
	libtool -static -o llvm.a $(SWIFT_STATIC_LIBS)
	$(call log_info,bundling LLVM xcframework)
	-rm -rf LLVM.xcframework
	xcodebuild -create-xcframework -library "./llvm.a" -headers "Headers" -output LLVM.xcframework

# Cleanup
clean-artifacts:
	-rm *.o
	-rm -rf CoreCompilerSupportLibs
	-rm -rf LLVM.xcframework
	-rm -rf Headers

clean: clean-artifacts
	$(call log_info,cleaning up)
	find . -mindepth 1 -maxdepth 1 \
		! -name Makefile \
		! -name LICENSE \
		! -name README.md \
		! -name .git \
		! -name .gitignore \
		! -name .github \
		! -name Scripts \
		! -name Patches \
		-exec rm -rf {} +
