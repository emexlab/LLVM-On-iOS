//===------ DarwinToolChains.cpp - Job invocations (Darwin-specific) ------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2025 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "ToolChains.h"

#include "swift/AST/DiagnosticsDriver.h"
#include "swift/AST/PlatformKindUtils.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Platform.h"
#include "swift/Basic/Range.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/TaskQueue.h"
#include "swift/Config.h"
#include "swift/Driver/Compilation.h"
#include "swift/Driver/Driver.h"
#include "swift/Driver/Job.h"
#include "swift/IDETool/CompilerInvocation.h"
#include "swift/Option/Options.h"
#include "clang/Basic/DarwinSDKInfo.h"
#include "clang/Basic/Version.h"
#include "clang/Driver/Util.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/VersionTuple.h"

using namespace swift;
using namespace swift::driver;
using namespace llvm::opt;
using namespace swift::driver::toolchains;

// =====================================================================
// Clang linker-driver passthrough helper.
//
// The Darwin toolchain now invokes `clang` as the linker driver instead
// of `ld` directly. Linker-only flags (ld64 spelling) must be wrapped as
// `-Wl,flag[,val1,val2,...]` so clang forwards them to ld64 unchanged.
//
// Usage:
//   addClangLinkerArg(Arguments, Args, "-rpath", {path});
//   addClangLinkerArg(Arguments, Args, "-platform_version",
//                     {platformName, osVer, sdkVer});
//   addClangLinkerArg(Arguments, Args, "-no_objc_category_merging");
// =====================================================================
static void addClangLinkerArg(ArgStringList &Arguments,
                              const ArgList &Args,
                              StringRef flag,
                              ArrayRef<StringRef> values = {}) {
  SmallString<128> buf;
  buf += "-Wl,";
  buf += flag;
  for (StringRef v : values) {
    buf += ',';
    buf += v;
  }
  Arguments.push_back(Args.MakeArgString(buf));
}

// Convenience overload taking a single value.
static void addClangLinkerArg(ArgStringList &Arguments,
                              const ArgList &Args,
                              StringRef flag,
                              StringRef value) {
  addClangLinkerArg(Arguments, Args, flag, ArrayRef<StringRef>{value});
}

std::string
toolchains::Darwin::findProgramRelativeToSwiftImpl(StringRef name) const {
  StringRef swiftPath = getDriver().getSwiftProgramPath();
  StringRef swiftBinDir = llvm::sys::path::parent_path(swiftPath);

  // See if we're in an Xcode toolchain.
  bool hasToolchain = false;
  llvm::SmallString<128> path{swiftBinDir};
  llvm::sys::path::remove_filename(path); // bin
  llvm::sys::path::remove_filename(path); // usr
  if (llvm::sys::path::extension(path) == ".xctoolchain") {
    hasToolchain = true;
    llvm::sys::path::remove_filename(path); // *.xctoolchain
    llvm::sys::path::remove_filename(path); // Toolchains
    llvm::sys::path::append(path, "usr", "bin");
  }

  StringRef paths[] = {swiftBinDir, path};
  auto pathsRef = llvm::ArrayRef(paths);
  if (!hasToolchain)
    pathsRef = pathsRef.drop_back();

  auto result = llvm::sys::findProgramByName(name, pathsRef);
  if (result)
    return result.get();
  return {};
}

ToolChain::InvocationInfo
toolchains::Darwin::constructInvocation(const InterpretJobAction &job,
                                        const JobContext &context) const {
  InvocationInfo II = ToolChain::constructInvocation(job, context);

  SmallVector<std::string, 4> runtimeLibraryPaths;
  getRuntimeLibraryPaths(runtimeLibraryPaths, context.Args, context.OI.SDKPath,
                         /*Shared=*/true);

  addPathEnvironmentVariableIfNeeded(II.ExtraEnvironment, "DYLD_LIBRARY_PATH",
                                     ":", options::OPT_L, context.Args,
                                     runtimeLibraryPaths);
  addPathEnvironmentVariableIfNeeded(II.ExtraEnvironment, "DYLD_FRAMEWORK_PATH",
                                     ":", options::OPT_F, context.Args,
                                     {"/System/Library/Frameworks"});
  // FIXME: Add options::OPT_Fsystem paths to DYLD_FRAMEWORK_PATH as well.
  return II;
}

static StringRef
getDarwinLibraryNameSuffixForTriple(const llvm::Triple &triple) {
  const DarwinPlatformKind kind = getDarwinPlatformKind(triple);
  switch (kind) {
  case DarwinPlatformKind::MacOS:
    return "osx";
  case DarwinPlatformKind::IPhoneOS:
    if (tripleIsMacCatalystEnvironment(triple))
        return "osx";
    return "ios";
  case DarwinPlatformKind::IPhoneOSSimulator:
    return "iossim";
  case DarwinPlatformKind::TvOS:
    return "tvos";
  case DarwinPlatformKind::TvOSSimulator:
    return "tvossim";
  case DarwinPlatformKind::WatchOS:
    return "watchos";
  case DarwinPlatformKind::WatchOSSimulator:
    return "watchossim";
  case DarwinPlatformKind::VisionOS:
    return "xros";
  case DarwinPlatformKind::VisionOSSimulator:
    return "xrossim";
  }
  llvm_unreachable("Unsupported Darwin platform");
}

std::string toolchains::Darwin::sanitizerRuntimeLibName(StringRef Sanitizer,
                                                        bool shared) const {
  return (Twine("libclang_rt.") + Sanitizer + "_" +
          getDarwinLibraryNameSuffixForTriple(this->getTriple()) +
          (shared ? "_dynamic.dylib" : ".a"))
      .str();
}

static void addLinkRuntimeLibRPath(const ArgList &Args,
                                   ArgStringList &Arguments,
                                   StringRef DarwinLibName,
                                   const ToolChain &TC) {
  assert(DarwinLibName.ends_with(".dylib") && "must be a dynamic library");

  // -rpath @executable_path
  addClangLinkerArg(Arguments, Args, "-rpath", "@executable_path");

  SmallString<128> ClangLibraryPath;
  TC.getClangLibraryPath(Args, ClangLibraryPath);

  // -rpath <clang-lib-path>
  addClangLinkerArg(Arguments, Args, "-rpath",
                    StringRef(ClangLibraryPath.str()));
}

static void addLinkSanitizerLibArgsForDarwin(const ArgList &Args,
                                             ArgStringList &Arguments,
                                             StringRef Sanitizer,
                                             const ToolChain &TC,
                                             bool shared = true) {
  // -lc++ / -lc++abi are clang-native; pass directly.
  Arguments.push_back("-lc++");
  Arguments.push_back("-lc++abi");

  auto LibName = TC.sanitizerRuntimeLibName(Sanitizer, shared);
  TC.addLinkRuntimeLib(Args, Arguments, LibName);

  if (shared)
    addLinkRuntimeLibRPath(Args, Arguments, LibName, TC);
}

static bool findXcodeClangPath(llvm::SmallVectorImpl<char> &path) {
  assert(path.empty());

  auto xcrunPath = llvm::sys::findProgramByName("xcrun");
  if (!xcrunPath.getError()) {
    const char *args[] = {"-toolchain", "default", "-f", "clang", nullptr};
    sys::TaskQueue queue;
    queue.addTask(xcrunPath->c_str(), args, /*Env=*/{},
                  /*Context=*/nullptr,
                  /*SeparateErrors=*/true);
    queue.execute(nullptr,
                  [&path](sys::ProcessId PID, int returnCode, StringRef output,
                          StringRef errors,
                          sys::TaskProcessInformation ProcInfo,
                          void *unused) -> sys::TaskFinishedResponse {
                    if (returnCode == 0) {
                      output = output.rtrim();
                      path.append(output.begin(), output.end());
                    }
                    return sys::TaskFinishedResponse::ContinueExecution;
                  });
  }

  return !path.empty();
}

static bool findXcodeClangLibPath(const Twine &libName,
                                  llvm::SmallVectorImpl<char> &path) {
  assert(path.empty());

  if (!findXcodeClangPath(path)) {
    return false;
  }
  llvm::sys::path::remove_filename(path); // 'clang'
  llvm::sys::path::remove_filename(path); // 'bin'
  llvm::sys::path::append(path, "lib", libName);
  return true;
}

static void addVersionString(const ArgList &inputArgs, ArgStringList &arguments,
                             llvm::VersionTuple version) {
  llvm::SmallString<8> buf;
  llvm::raw_svector_ostream os{buf};
  os << version.getMajor() << '.' << version.getMinor().value_or(0) << '.'
     << version.getSubminor().value_or(0);
  arguments.push_back(inputArgs.MakeArgString(os.str()));
}

// Render a version tuple to a heap-stable string we can pass into a
// `-Wl,...` token via addClangLinkerArg.
static std::string versionString(llvm::VersionTuple version) {
  std::string s;
  llvm::raw_string_ostream os(s);
  os << version.getMajor() << '.' << version.getMinor().value_or(0) << '.'
     << version.getSubminor().value_or(0);
  return os.str();
}

void
toolchains::Darwin::addLinkerInputArgs(InvocationInfo &II,
                                       const JobContext &context) const {
  ArgStringList &Arguments = II.Arguments;
  if (context.shouldUseInputFileList()) {
    // -filelist is a clang flag too, forwarded to ld; no wrapping needed.
    Arguments.push_back("-filelist");
    Arguments.push_back(context.getTemporaryFilePath("inputs", "LinkFileList"));
    II.FilelistInfos.push_back(
        {Arguments.back(), context.OI.CompilerOutputType,
         FilelistInfo::WhichFiles::InputJobsAndSourceInputActions});
  } else {
    addPrimaryInputsOfType(Arguments, context.Inputs, context.Args,
                           file_types::TY_Object);
    addPrimaryInputsOfType(Arguments, context.Inputs, context.Args,
                           file_types::TY_TBD);
    addPrimaryInputsOfType(Arguments, context.Inputs, context.Args,
                           file_types::TY_LLVM_BC);
    addInputsOfType(Arguments, context.InputActions, file_types::TY_Object);
    addInputsOfType(Arguments, context.InputActions, file_types::TY_TBD);
    addInputsOfType(Arguments, context.InputActions, file_types::TY_LLVM_BC);
  }

  // -add_ast_path is ld64-only. The addInputsOfType / addPrimaryInputsOfType
  // overloads here push it as a flag prefix per swiftmodule input. Since the
  // helper interleaves "-add_ast_path <path>" pairs with no opportunity to
  // intercept the flag spelling, we need to wrap them after the fact. Walk
  // the most recent suffix of `Arguments` and rewrite each pair.
  //
  // To keep things simple, capture the size before, then do the appends, then
  // rewrite.
  auto rewriteAddAstPath = [&](size_t startIdx) {
    ArgStringList rewritten;
    for (size_t i = 0; i < startIdx; ++i)
      rewritten.push_back(Arguments[i]);
    for (size_t i = startIdx; i < Arguments.size(); ) {
      StringRef tok = Arguments[i];
      if (tok == "-add_ast_path" && i + 1 < Arguments.size()) {
        addClangLinkerArg(rewritten, context.Args, "-add_ast_path",
                          StringRef(Arguments[i + 1]));
        i += 2;
      } else {
        rewritten.push_back(Arguments[i]);
        ++i;
      }
    }
    Arguments = std::move(rewritten);
  };

  size_t before = Arguments.size();

  if (context.OI.CompilerMode == OutputInfo::Mode::SingleCompile)
    addInputsOfType(Arguments, context.Inputs, context.Args,
                    file_types::TY_SwiftModuleFile, "-add_ast_path");
  else
    addPrimaryInputsOfType(Arguments, context.Inputs, context.Args,
                           file_types::TY_SwiftModuleFile, "-add_ast_path");

  addInputsOfType(Arguments, context.InputActions,
                  file_types::TY_SwiftModuleFile, "-add_ast_path");

  rewriteAddAstPath(before);
}

void toolchains::Darwin::addLTOLibArgs(ArgStringList &Arguments,
                                       const JobContext &context) const {
  if (!context.OI.LibLTOPath.empty()) {
    addClangLinkerArg(Arguments, context.Args, "-lto_library",
                      StringRef(context.OI.LibLTOPath));
  } else {
    StringRef P = llvm::sys::path::parent_path(getDriver().getSwiftProgramPath());
    llvm::SmallString<128> LibLTOPath(P);
    llvm::sys::path::remove_filename(LibLTOPath); // Remove '/bin'
    llvm::sys::path::append(LibLTOPath, "lib");
    llvm::sys::path::append(LibLTOPath, "libLTO.dylib");
    if (llvm::sys::fs::exists(LibLTOPath)) {
      addClangLinkerArg(Arguments, context.Args, "-lto_library",
                        StringRef(LibLTOPath.str()));
    } else {
      llvm::SmallString<128> LibLTOPath2;
      if (findXcodeClangLibPath("libLTO.dylib", LibLTOPath2)) {
        addClangLinkerArg(Arguments, context.Args, "-lto_library",
                          StringRef(LibLTOPath2.str()));
      }
    }
  }
}

void
toolchains::Darwin::addSanitizerArgs(ArgStringList &Arguments,
                                     const DynamicLinkJobAction &job,
                                     const JobContext &context) const {
  if (context.OI.SelectedSanitizers & SanitizerKind::Address) {
    if (context.OI.SanitizerUseStableABI)
      addLinkSanitizerLibArgsForDarwin(context.Args, Arguments, "asan_abi",
                                       *this, false);
    else
      addLinkSanitizerLibArgsForDarwin(context.Args, Arguments, "asan", *this);
  }

  if (context.OI.SelectedSanitizers & SanitizerKind::Thread)
    addLinkSanitizerLibArgsForDarwin(context.Args, Arguments, "tsan", *this);

  if (context.OI.SelectedSanitizers & SanitizerKind::Undefined)
    addLinkSanitizerLibArgsForDarwin(context.Args, Arguments, "ubsan", *this);

  if (job.getKind() == LinkKind::Executable &&
      (context.OI.SelectedSanitizers & SanitizerKind::Fuzzer))
    addLinkSanitizerLibArgsForDarwin(context.Args, Arguments, "fuzzer", *this,
                                     /*shared=*/false);
}

namespace {

enum class BackDeployLibFilter {
  executable,
  all
};

bool jobMatchesFilter(LinkKind jobKind, BackDeployLibFilter filter) {
  switch (filter) {
  case BackDeployLibFilter::executable:
    return jobKind == LinkKind::Executable;
  case BackDeployLibFilter::all:
    return true;
  }
  llvm_unreachable("unhandled back deploy lib filter!");
}

}

void
toolchains::Darwin::addArgsToLinkStdlib(ArgStringList &Arguments,
                                        const DynamicLinkJobAction &job,
                                        const JobContext &context) const {

  SmallString<128> SharedResourceDirPath;
  getResourceDirPath(SharedResourceDirPath, context.Args, /*Shared=*/true);
  std::optional<llvm::VersionTuple> runtimeCompatibilityVersion;

  if (context.Args.hasArg(options::OPT_runtime_compatibility_version)) {
    auto value = context.Args.getLastArgValue(
                                    options::OPT_runtime_compatibility_version);
    if (value == "5.0") {
      runtimeCompatibilityVersion = llvm::VersionTuple(5, 0);
    } else if (value == "5.1") {
      runtimeCompatibilityVersion = llvm::VersionTuple(5, 1);
    } else if (value == "5.5") {
      runtimeCompatibilityVersion = llvm::VersionTuple(5, 5);
    } else if (value == "5.6") {
      runtimeCompatibilityVersion = llvm::VersionTuple(5, 6);
    } else if (value == "5.8") {
      runtimeCompatibilityVersion = llvm::VersionTuple(5, 8);
    } else if (value == "6.0") {
      runtimeCompatibilityVersion = llvm::VersionTuple(6, 0);
    } else if (value == "6.2") {
      runtimeCompatibilityVersion = llvm::VersionTuple(6, 2);
    } else if (value == "none") {
      runtimeCompatibilityVersion = std::nullopt;
    } else {
      // TODO: diagnose unknown runtime compatibility version?
    }
  } else if (job.getKind() == LinkKind::Executable) {
    runtimeCompatibilityVersion
                   = getSwiftRuntimeCompatibilityVersionForTarget(getTriple());
  }

  if (runtimeCompatibilityVersion) {
    auto addBackDeployLib = [&](llvm::VersionTuple version,
                                BackDeployLibFilter filter,
                                StringRef libraryName,
                                bool forceLoad) {
      if (*runtimeCompatibilityVersion > version)
        return;

      if (!jobMatchesFilter(job.getKind(), filter))
        return;

      SmallString<128> BackDeployLib;
      BackDeployLib.append(SharedResourceDirPath);
      llvm::sys::path::append(BackDeployLib, "lib" + libraryName + ".a");

      if (llvm::sys::fs::exists(BackDeployLib)) {
        if (forceLoad) {
          // -force_load is ld64-only.
          addClangLinkerArg(Arguments, context.Args, "-force_load",
                            StringRef(BackDeployLib.str()));
        } else {
          // Bare path is a regular linker input; clang accepts directly.
          Arguments.push_back(context.Args.MakeArgString(BackDeployLib));
        }
      }
    };

    #define BACK_DEPLOYMENT_LIB(Version, Filter, LibraryName, ForceLoad) \
      addBackDeployLib(                                                  \
          llvm::VersionTuple Version, BackDeployLibFilter::Filter,       \
          LibraryName, ForceLoad);
    #include "swift/Frontend/BackDeploymentLibs.def"
  }

  SmallVector<std::string, 4> RuntimeLibPaths;
  getRuntimeLibraryPaths(RuntimeLibPaths, context.Args,
                         context.OI.SDKPath, /*Shared=*/true);

  // -L is clang-native.
  for (auto path : RuntimeLibPaths) {
    Arguments.push_back("-L");
    Arguments.push_back(context.Args.MakeArgString(path));
  }

  if (context.Args.hasFlag(options::OPT_toolchain_stdlib_rpath,
                           options::OPT_no_toolchain_stdlib_rpath, false)) {
    for (auto path : RuntimeLibPaths) {
      addClangLinkerArg(Arguments, context.Args, "-rpath", StringRef(path));
    }
  } else if (!tripleRequiresRPathForSwiftLibrariesInOS(getTriple()) ||
             context.Args.hasArg(options::OPT_no_stdlib_rpath)) {
    // No rpath needed.
  } else {
    addClangLinkerArg(Arguments, context.Args, "-rpath", "/usr/lib/swift");
  }
}

void
toolchains::Darwin::addProfileGenerationArgs(ArgStringList &Arguments,
                                             const JobContext &context) const {
  const llvm::Triple &Triple = getTriple();
  if (needsInstrProfileRuntime(context.Args)) {
    SmallString<128> LibProfile;
    getClangLibraryPath(context.Args, LibProfile);

    StringRef RT;
    if (Triple.isiOS()) {
      if (Triple.isTvOS())
        RT = "tvos";
      else
        RT = "ios";
    } else if (Triple.isWatchOS()) {
      RT = "watchos";
    } else if (Triple.isXROS()) {
      RT = "xros";
    } else {
      assert(Triple.isMacOSX());
      RT = "osx";
    }

    StringRef Sim;
    if (Triple.isSimulatorEnvironment()) {
      Sim = "sim";
    }

    llvm::sys::path::append(LibProfile,
                            "libclang_rt.profile_" + RT + Sim + ".a");

    if (!Sim.empty() && !llvm::sys::fs::exists(LibProfile)) {
      llvm::sys::path::remove_filename(LibProfile);
      llvm::sys::path::append(LibProfile, "libclang_rt.profile_" + RT + ".a");
    }

    // Bare static-archive path; clang treats as a regular linker input.
    Arguments.push_back(context.Args.MakeArgString(LibProfile));
  }
}

std::optional<llvm::VersionTuple>
toolchains::Darwin::getTargetSDKVersion(const llvm::Triple &triple) const {
  if (!SDKInfo)
    return std::nullopt;
  return swift::getTargetSDKVersion(*SDKInfo, triple);
}

void
toolchains::Darwin::addDeploymentTargetArgs(ArgStringList &Arguments,
                                            const JobContext &context) const {
  auto addPlatformVersionArg = [&](const llvm::Triple &triple) {
    const char *platformName;
    if (tripleIsMacCatalystEnvironment(triple)) {
      platformName = "mac-catalyst";
    } else {
      switch (getDarwinPlatformKind(triple)) {
      case DarwinPlatformKind::MacOS:
        platformName = "macos"; break;
      case DarwinPlatformKind::IPhoneOS:
        platformName = "ios"; break;
      case DarwinPlatformKind::IPhoneOSSimulator:
        platformName = "ios-simulator"; break;
      case DarwinPlatformKind::TvOS:
        platformName = "tvos"; break;
      case DarwinPlatformKind::TvOSSimulator:
        platformName = "tvos-simulator"; break;
      case DarwinPlatformKind::WatchOS:
        platformName = "watchos"; break;
      case DarwinPlatformKind::WatchOSSimulator:
        platformName = "watchos-simulator"; break;
      case DarwinPlatformKind::VisionOS:
        platformName = "xros"; break;
      case DarwinPlatformKind::VisionOSSimulator:
        platformName = "xros-simulator"; break;
      }
    }

    llvm::VersionTuple osVersion;
    if (tripleIsMacCatalystEnvironment(triple)) {
      osVersion = triple.getiOSVersion();

      if (osVersion.getMajor() < 14 && triple.isAArch64()) {
        osVersion = llvm::VersionTuple(/*Major=*/14, /*Minor=*/0);
      } else if (osVersion.getMajor() < 13) {
        osVersion = llvm::VersionTuple(/*Major=*/13, /*Minor=*/1);
      }
    } else {
      switch (getDarwinPlatformKind((triple))) {
      case DarwinPlatformKind::MacOS:
        triple.getMacOSXVersion(osVersion);
        if (triple.isAArch64() && osVersion.getMajor() <= 10 &&
            osVersion.getMinor().value_or(0) < 16) {
          osVersion = llvm::VersionTuple(/*Major=*/10, /*Minor=*/16);
          osVersion = canonicalizePlatformVersion(PlatformKind::macOS,
                                                  osVersion);
        }
        break;
      case DarwinPlatformKind::IPhoneOS:
      case DarwinPlatformKind::IPhoneOSSimulator:
      case DarwinPlatformKind::TvOS:
      case DarwinPlatformKind::TvOSSimulator:
        osVersion = triple.getiOSVersion();
        if (triple.isSimulatorEnvironment() && triple.isAArch64() &&
            osVersion.getMajor() < 14) {
          osVersion = llvm::VersionTuple(/*Major=*/14, /*Minor=*/0);
        }
        break;
      case DarwinPlatformKind::WatchOS:
      case DarwinPlatformKind::WatchOSSimulator:
        osVersion = triple.getOSVersion();
        break;
      case DarwinPlatformKind::VisionOS:
      case DarwinPlatformKind::VisionOSSimulator:
        osVersion = triple.getOSVersion();
        if (triple.isArch64Bit() && triple.isSimulatorEnvironment() &&
            osVersion.getMajor() < 1) {
          osVersion = llvm::VersionTuple(/*Major=*/1, /*Minor=*/0);
        }
        break;
      }
    }

    auto sdkVersion = getTargetSDKVersion(triple)
        .value_or(llvm::VersionTuple());

    // -platform_version <name> <ver> <sdkver> — ld64-only, wrap as one
    // -Wl, token with three comma-separated values.
    std::string osVerStr  = versionString(osVersion);
    std::string sdkVerStr = versionString(sdkVersion);
    addClangLinkerArg(Arguments, context.Args, "-platform_version",
                      {StringRef(platformName),
                       StringRef(osVerStr),
                       StringRef(sdkVerStr)});
  };

  addPlatformVersionArg(getTriple());

  if (auto targetVariant = getTargetVariant()) {
    assert(triplesAreValidForZippering(getTriple(), *targetVariant));
    addPlatformVersionArg(*targetVariant);
  }
}

static unsigned getDWARFVersionForTriple(const llvm::Triple &triple) {
  llvm::VersionTuple osVersion;
  const DarwinPlatformKind kind = getDarwinPlatformKind(triple);
  switch (kind) {
  case DarwinPlatformKind::MacOS:
    triple.getMacOSXVersion(osVersion);
    if (osVersion < llvm::VersionTuple(10, 11))
      return 2;
    if (osVersion < llvm::VersionTuple(15))
      return 4;
    return 5;
  case DarwinPlatformKind::IPhoneOSSimulator:
  case DarwinPlatformKind::IPhoneOS:
  case DarwinPlatformKind::TvOS:
  case DarwinPlatformKind::TvOSSimulator:
    osVersion = triple.getiOSVersion();
   if (osVersion < llvm::VersionTuple(9))
     return 2;
    if (osVersion < llvm::VersionTuple(18))
      return 4;
    return 5;
  case DarwinPlatformKind::WatchOS:
  case DarwinPlatformKind::WatchOSSimulator:
    osVersion = triple.getWatchOSVersion();
    if (osVersion < llvm::VersionTuple(11))
      return 4;
    return 5;
  case DarwinPlatformKind::VisionOS:
  case DarwinPlatformKind::VisionOSSimulator:
    osVersion = triple.getOSVersion();
    if (osVersion < llvm::VersionTuple(2))
      return 4;
    return 5;
  }
  llvm_unreachable("unsupported platform kind");
}

void toolchains::Darwin::addCommonFrontendArgs(
    const OutputInfo &OI, const CommandOutput &output,
    const llvm::opt::ArgList &inputArgs,
    llvm::opt::ArgStringList &arguments) const {
  ToolChain::addCommonFrontendArgs(OI, output, inputArgs, arguments);

  if (auto sdkVersion = getTargetSDKVersion(getTriple())) {
    arguments.push_back("-target-sdk-version");
    arguments.push_back(inputArgs.MakeArgString(sdkVersion->getAsString()));
  }

  if (auto targetVariant = getTargetVariant()) {
    if (auto variantSDKVersion = getTargetSDKVersion(*targetVariant)) {
      arguments.push_back("-target-variant-sdk-version");
      arguments.push_back(
          inputArgs.MakeArgString(variantSDKVersion->getAsString()));
    }
  }
  std::string dwarfVersion;
  {
    llvm::raw_string_ostream os(dwarfVersion);
    os << "-dwarf-version=";
    if (OI.DWARFVersion)
      os << std::to_string(*OI.DWARFVersion);
    else
      os << getDWARFVersionForTriple(getTriple());
  }
  arguments.push_back(inputArgs.MakeArgString(dwarfVersion));
}

static void addExternalPluginFrontendArgs(
    StringRef basePath, const llvm::opt::ArgList &inputArgs,
    llvm::opt::ArgStringList &arguments) {
  SmallString<128> pluginServer;
  llvm::sys::path::append(
      pluginServer, basePath, "usr", "bin", "swift-plugin-server");

  SmallString<128> pluginDir;
  llvm::sys::path::append(pluginDir, basePath, "usr", "lib");
  llvm::sys::path::append(pluginDir, "swift", "host", "plugins");
  arguments.push_back("-external-plugin-path");
  arguments.push_back(inputArgs.MakeArgString(pluginDir + "#" + pluginServer));

  pluginDir.clear();
  llvm::sys::path::append(pluginDir, basePath, "usr", "local", "lib");
  llvm::sys::path::append(pluginDir, "swift", "host", "plugins");
  arguments.push_back("-external-plugin-path");
  arguments.push_back(inputArgs.MakeArgString(pluginDir + "#" + pluginServer));
}

void toolchains::Darwin::addPlatformSpecificPluginFrontendArgs(
    const OutputInfo &OI,
    const CommandOutput &output,
    const llvm::opt::ArgList &inputArgs,
    llvm::opt::ArgStringList &arguments) const {
  if (!OI.SDKPath.empty()) {
    addExternalPluginFrontendArgs(OI.SDKPath, inputArgs, arguments);
  }

  if (!OI.SDKPath.empty()) {
    SmallString<128> platformPath;
    llvm::sys::path::append(platformPath, OI.SDKPath);
    llvm::sys::path::remove_filename(platformPath); // specific SDK
    llvm::sys::path::remove_filename(platformPath); // SDKs
    llvm::sys::path::remove_filename(platformPath); // Developer

    StringRef platformName = llvm::sys::path::filename(platformPath);
    if (platformName.ends_with("Simulator.platform")){
      StringRef devicePlatformName =
          platformName.drop_back(strlen("Simulator.platform"));
      llvm::sys::path::remove_filename(platformPath); // Platform
      llvm::sys::path::append(platformPath, devicePlatformName + "OS.platform");
    }

    llvm::sys::path::append(platformPath, "Developer");
    addExternalPluginFrontendArgs(platformPath, inputArgs, arguments);
  }
}

ToolChain::InvocationInfo
toolchains::Darwin::constructInvocation(const DynamicLinkJobAction &job,
                                        const JobContext &context) const {
  assert(context.Output.getPrimaryOutputType() == file_types::TY_Image &&
         "Invalid linker output type.");

  if (context.Args.hasFlag(options::OPT_static_executable,
                           options::OPT_no_static_executable, false)) {
    llvm::report_fatal_error("-static-executable is not supported on Darwin");
  }

  const llvm::Triple &Triple = getTriple();

  // Use clang as the linker driver instead of invoking ld directly.
  // This matches the behavior of swift-driver and Xcode, and lets us
  // express linker flags in clang-driver form (-Wl,...) so we don't have
  // to replicate clang's logic for compiler-rt, sysroot, etc.
  const char *Clang = "clang";
  if (const Arg *A = context.Args.getLastArg(options::OPT_tools_directory)) {
    StringRef toolchainPath(A->getValue());
    if (auto toolchainClang =
            llvm::sys::findProgramByName("clang", {toolchainPath})) {
      Clang = context.Args.MakeArgString(toolchainClang.get());
    }
  }

  InvocationInfo II = {Clang};
  ArgStringList &Arguments = II.Arguments;

  addLinkerInputArgs(II, context);

  switch (job.getKind()) {
  case LinkKind::None:
    llvm_unreachable("invalid link kind");
  case LinkKind::Executable:
    // No extra flag for executables.
    break;
  case LinkKind::DynamicLibrary:
    // clang-native flag (it translates to ld's -dylib internally).
    Arguments.push_back("-dynamiclib");
    break;
  case LinkKind::StaticLibrary:
    llvm_unreachable("the dynamic linker cannot build static libraries");
  }

  assert(Triple.isOSDarwin());

  // Always link the regular compiler_rt if it's present.
  // Bare archive path; clang accepts as a linker input.
  SmallString<128> CompilerRTPath;
  getClangLibraryPath(context.Args, CompilerRTPath);
  llvm::sys::path::append(
      CompilerRTPath,
      Twine("libclang_rt.") +
        getDarwinLibraryNameSuffixForTriple(Triple) +
        ".a");
  if (llvm::sys::fs::exists(CompilerRTPath))
    Arguments.push_back(context.Args.MakeArgString(CompilerRTPath));

  if (job.shouldPerformLTO()) {
    addLTOLibArgs(Arguments, context);
  }

  // -F is clang-native.
  for (const Arg *arg :
       context.Args.filtered(options::OPT_F, options::OPT_Fsystem)) {
    Arguments.push_back("-F");
    Arguments.push_back(arg->getValue());
  }

  if (context.Args.hasArg(options::OPT_enable_app_extension)) {
    // clang-native (translates to ld's -application_extension).
    Arguments.push_back("-fapplication-extension");
  }

  addSanitizerArgs(Arguments, job, context);

  if (context.Args.hasArg(options::OPT_embed_bitcode,
                          options::OPT_embed_bitcode_marker)) {
    // clang-native (translates to ld's -bitcode_bundle).
    Arguments.push_back("-fembed-bitcode");
  }

  if (!context.OI.SDKPath.empty()) {
    // -isysroot is clang-native; clang itself uses it for resource lookup
    // and also forwards it to ld as -syslibroot.
    Arguments.push_back("-isysroot");
    Arguments.push_back(context.Args.MakeArgString(context.OI.SDKPath));
  }

  Arguments.push_back("-lobjc");
  Arguments.push_back("-lSystem");

  // -arch is clang-native.
  Arguments.push_back("-arch");
  Arguments.push_back(context.Args.MakeArgString(getTriple().getArchName()));

  if (context.Args.hasArg(options::OPT_enable_experimental_cxx_interop)) {
    Arguments.push_back("-lc++");
  }

  addArgsToLinkStdlib(Arguments, job, context);

  addProfileGenerationArgs(Arguments, context);
  addDeploymentTargetArgs(Arguments, context);

  // ld64-only flag.
  addClangLinkerArg(Arguments, context.Args, "-no_objc_category_merging");

  // These custom arguments should be right before the object file at the end.
  context.Args.AddAllArgsExcept(Arguments, {options::OPT_linker_option_Group},
                                {options::OPT_l});
  ToolChain::addLinkedLibArgs(context.Args, Arguments);

  // User-supplied -Xlinker pass-through. Clang understands -Xlinker natively,
  // so we keep emitting it as -Xlinker <val> pairs.
  for (const Arg *A : context.Args.filtered(options::OPT_Xlinker)) {
    for (const char *val : A->getValues()) {
      Arguments.push_back("-Xlinker");
      Arguments.push_back(val);
    }
  }

  // -o and the output path. clang-native.
  Arguments.push_back("-o");
  Arguments.push_back(
      context.Args.MakeArgString(context.Output.getPrimaryOutputFilename()));

  return II;
}


ToolChain::InvocationInfo
toolchains::Darwin::constructInvocation(const StaticLinkJobAction &job,
                                        const JobContext &context) const {
   assert(context.Output.getPrimaryOutputType() == file_types::TY_Image &&
         "Invalid linker output type.");

  // Static archives still go through libtool — unchanged.
  const char *LibTool = "libtool";

  InvocationInfo II = {LibTool};
  ArgStringList &Arguments = II.Arguments;

  Arguments.push_back("-static");

  if (context.shouldUseInputFileList()) {
    Arguments.push_back("-filelist");
    Arguments.push_back(context.getTemporaryFilePath("inputs", "LinkFileList"));
    II.FilelistInfos.push_back({Arguments.back(), context.OI.CompilerOutputType,
                                FilelistInfo::WhichFiles::InputJobs});
  } else {
    addPrimaryInputsOfType(Arguments, context.Inputs, context.Args,
                           file_types::TY_Object);
    addPrimaryInputsOfType(Arguments, context.Inputs, context.Args,
                           file_types::TY_LLVM_BC);
  }

  addInputsOfType(Arguments, context.InputActions, file_types::TY_Object);
  addInputsOfType(Arguments, context.InputActions, file_types::TY_LLVM_BC);

  Arguments.push_back("-o");

  Arguments.push_back(
      context.Args.MakeArgString(context.Output.getPrimaryOutputFilename()));

  return II;
}

bool toolchains::Darwin::shouldStoreInvocationInDebugInfo() const {
  if (const char *S = ::getenv("RC_DEBUG_OPTIONS"))
    return S[0] != '\0';
  return false;
}

std::string toolchains::Darwin::getGlobalDebugPathRemapping() const {
  if (const char *S = ::getenv("RC_DEBUG_PREFIX_MAP"))
    return S;
  return {};
}

static void validateDeploymentTarget(const toolchains::Darwin &TC,
                                     DiagnosticEngine &diags,
                                     const llvm::opt::ArgList &args) {
  auto triple = TC.getTriple();
  if (triple.isMacOSX()) {
    if (triple.isMacOSXVersionLT(10, 9))
      diags.diagnose(SourceLoc(), diag::error_os_minimum_deployment,
                     "OS X 10.9");
  } else if (triple.isiOS()) {
    if (triple.isTvOS()) {
      if (triple.isOSVersionLT(9, 0)) {
        diags.diagnose(SourceLoc(), diag::error_os_minimum_deployment,
                       "tvOS 9.0");
        return;
      }
    }
    if (triple.isOSVersionLT(7))
      diags.diagnose(SourceLoc(), diag::error_os_minimum_deployment,
                     "iOS 7");
    if (triple.isArch32Bit() && !triple.isOSVersionLT(11)) {
      diags.diagnose(SourceLoc(), diag::error_ios_maximum_deployment_32,
                     triple.getOSMajorVersion());
    }
  } else if (triple.isWatchOS()) {
    if (triple.isOSVersionLT(2, 0)) {
      diags.diagnose(SourceLoc(), diag::error_os_minimum_deployment,
                     "watchOS 2.0");
      return;
    }
  }
}

static void validateTargetVariant(const toolchains::Darwin &TC,
                                  DiagnosticEngine &diags,
                                  const llvm::opt::ArgList &args,
                                  StringRef defaultTarget) {
  if (TC.getTargetVariant().has_value()) {
    auto target = TC.getTriple();
    auto variant = *TC.getTargetVariant();

    if (!triplesAreValidForZippering(target, variant)) {
      diags.diagnose(SourceLoc(), diag::error_unsupported_target_variant,
                    variant.str(),
                    variant.isiOS());
    }
  }
}

void
toolchains::Darwin::validateArguments(DiagnosticEngine &diags,
                                      const llvm::opt::ArgList &args,
                                      StringRef defaultTarget) const {
  validateDeploymentTarget(*this, diags, args);
  validateTargetVariant(*this, diags, args, defaultTarget);

  if (args.hasArg(options::OPT_static_stdlib)) {
    diags.diagnose(SourceLoc(), diag::error_darwin_static_stdlib_not_supported);
  }

  if (args.hasArg(options::OPT_link_objc_runtime,
                  options::OPT_no_link_objc_runtime)) {
    diags.diagnose(SourceLoc(), diag::warn_darwin_link_objc_deprecated);
  }
}

void
toolchains::Darwin::validateOutputInfo(DiagnosticEngine &diags,
                                       const OutputInfo &outputInfo) const {
  if (!outputInfo.SDKPath.empty()) {
    auto SDKInfoOrErr = clang::parseDarwinSDKInfo(
        *llvm::vfs::getRealFileSystem(), outputInfo.SDKPath);
    if (SDKInfoOrErr) {
      SDKInfo = *SDKInfoOrErr;
    } else {
      llvm::consumeError(SDKInfoOrErr.takeError());
      diags.diagnose(SourceLoc(), diag::warn_drv_darwin_sdk_invalid_settings);
    }
  }
}
