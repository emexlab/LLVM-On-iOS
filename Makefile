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
all: CoreCompiler.framework/CoreCompiler

# Fetch & Build Swift and LLVM for iOS
swift:
	$(call log_info,fetching swift sources ($(SWIFT_BRANCH)))
	SWIFT_BRANCH="$(SWIFT_BRANCH)" SWIFT_SOURCE_DIR="$(SWIFT_SOURCE_DIR)" Scripts/build-swift-toolchain.sh fetch
	$(call log_info,bypassing lld darwin incompatibility)
	perl -i -0pe 's|(// Swift LLVM fork downstream change start\n)(.*?)(// Swift LLVM fork downstream change end\n)|$$1/* NYXIAN: apple lies, lld works fine for MachO\n$$2*/\n$$3|s' \
	llvm-project/lld/MachO/InputFiles.cpp

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
SWIFT_LINK_PATHS = -L$(SWIFT_TOOLCHAIN_ROOT)/lib \
                   -L$(SWIFT_TOOLCHAIN_ROOT)/lib/swift/iphoneos \
                   -L$(SWIFT_TOOLCHAIN_ROOT)/lib/swift/iphoneos/$(APPLE_ARCH) \
                   -L$(SWIFT_TOOLCHAIN_ROOT)/lib/swift/host/compiler
INC = -ISource -ISwiftToolchain-iphoneos/include -Illvm-project/lld/include -Illvm-project/clang/include -Illvm-project/llvm/include -Ibuild/LLVMClangSwift_iphoneos/llvm-iphoneos-arm64/tools/clang/include -Iswift/include -Iswift/stdlib/public/SwiftShims

CoreCompiler.framework/CoreCompiler: swift-toolchain
	$(call log_info,building CoreCompiler framework)
	-rm -rf CoreCompilerSupportLibs
	-rm *.o
	clang -c -target $(TARGET_TRIPLE) -isysroot $(SDK) $(INC) Source/CoreCompiler/*.c
	clang -c -fobjc-arc -ObjC -target $(TARGET_TRIPLE) -isysroot $(SDK) $(INC) Source/CoreCompiler/*.m
	clang++ -c -Wno-elaborated-enum-base -std=c++17 -target $(TARGET_TRIPLE) -isysroot $(SDK) $(INC) Source/CoreCompiler/*.cpp
	clang++ -fobjc-arc -fno-rtti -fno-exceptions -fvisibility=hidden -fvisibility-inlines-hidden -ffunction-sections -fdata-sections -Wl,-dead_strip -flto=full -Os -fno-exceptions -Wl,-x -Wl,-S -Wl,-dead_strip_dylibs -ObjC -target $(TARGET_TRIPLE) -isysroot $(SDK) $(SWIFT_LINK_PATHS) *.o $(SWIFT_STATIC_LIBS) $(SWIFT_HOST_COMPILER_DYLIBS) -framework CoreFoundation -lz -lxml2 -lswiftCore -o CoreCompiler.framework/CoreCompiler -shared -fPIC -install_name @rpath/CoreCompiler.framework/CoreCompiler
	-rm *.o
	-rm -rf CoreCompiler.framework/Headers
	mkdir -p CoreCompiler.framework/Headers
	cp Source/CoreCompiler/*.h CoreCompiler.framework/Headers/
	mkdir CoreCompilerSupportLibs
	cp $(SWIFT_HOST_COMPILER_DYLIBS) CoreCompilerSupportLibs/

# Cleanup
clean-artifacts:
	-rm *.o
	-rm -rf CoreCompilerSupportLibs
	-rm CoreCompiler.framework/CoreCompiler
	-rm CoreCompiler.framework/Headers/*

clean: clean-artifacts
	$(call log_info,cleaning up)
	find . -mindepth 1 -maxdepth 1 \
		! -name Makefile \
		! -name LICENSE \
		! -name README.md \
		! -name .git \
		! -name .gitignore \
		! -name .github \
		! -name CoreCompiler.framework \
		! -name Source \
		! -name Scripts \
		-exec rm -rf {} +
