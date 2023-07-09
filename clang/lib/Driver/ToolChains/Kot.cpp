#include "Kot.h"
#include "CommonArgs.h"
#include "llvm/Support/Path.h"
#include "llvm/Option/ArgList.h"
#include "clang/Driver/Driver.h"
#include "clang/Config/config.h"
#include "clang/Driver/Options.h"
#include "llvm/Support/FileSystem.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/SanitizerArgs.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "llvm/Support/VirtualFileSystem.h"


using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang::driver::tools;
using namespace clang;
using namespace llvm::opt;

using tools::addMultilibFlag;

void kot::Linker::ConstructJob(Compilation &C, const JobAction &JA, const InputInfo &Output, const InputInfoList &Inputs, const ArgList &Args, const char *LinkingOutput) const{
  const toolchains::Kot &ToolChain = static_cast<const toolchains::Kot&>(getToolChain());
  const Driver &D = ToolChain.getDriver();

  // Silence warning for "clang -g foo.o -o foo"
  Args.ClaimAllArgs(options::OPT_g_Group);
  // and "clang -emit-llvm foo.o -o foo"
  Args.ClaimAllArgs(options::OPT_emit_llvm);
  // and for "clang -w foo.o -o foo"
  Args.ClaimAllArgs(options::OPT_w);
  // Other warning options are already handled somewhere else.

  ArgStringList CmdArgs;

  const char* Exec = Args.MakeArgString(ToolChain.GetLinkerPath());

  if(!D.SysRoot.empty()){
    CmdArgs.push_back(Args.MakeArgString("--sysroot=" + D.SysRoot));
  }

  // force static
  CmdArgs.push_back("-static");

  CmdArgs.push_back("-o");
  CmdArgs.push_back(Output.getFilename());

  if(!Args.hasArg(options::OPT_nostdlib, options::OPT_nostartfiles)){
    if(!Args.hasArg(options::OPT_shared)){
      if(!D.SysRoot.empty()){
        SmallString<128> P(D.SysRoot);
        llvm::sys::path::append(P, "lib/crt0.o");
        CmdArgs.push_back(Args.MakeArgString(P));
      }
    }
  }

  Args.AddAllArgs(CmdArgs, options::OPT_L);
  Args.AddAllArgs(CmdArgs, options::OPT_u);

  ToolChain.AddFilePathLibArgs(Args, CmdArgs);

  if(D.isUsingLTO()){
    assert(!Inputs.empty() && "Must have at least one input");
    addLTOOptions(ToolChain, Args, CmdArgs, Output, Inputs[0], D.getLTOMode() == LTOK_Thin);
  }

  AddLinkerInputs(ToolChain, Inputs, Args, CmdArgs, JA);

  if(!Args.hasArg(options::OPT_nostdlib, options::OPT_nodefaultlibs)){
    if(!Args.hasArg(options::OPT_nolibc)){
      CmdArgs.push_back("-lc");
    }
  }

  C.addCommand(std::make_unique<Command>(JA, *this, ResponseFileSupport::AtFileUTF8(), Exec, CmdArgs, Inputs, Output));
}

Kot::Kot(const Driver &D, const llvm::Triple &Triple, const ArgList &Args):ToolChain(D, Triple, Args){
  if(!D.SysRoot.empty()){
    SmallString<128> P(D.SysRoot);
    llvm::sys::path::append(P, "lib");
    getFilePaths().push_back(std::string(P.str()));
  }
}

std::string Kot::ComputeEffectiveClangTriple(const ArgList &Args, types::ID InputType) const{
  llvm::Triple Triple(ComputeLLVMTriple(Args, InputType));
  return Triple.str();
}

Tool* Kot::buildLinker() const{
  return new tools::kot::Linker(*this);
}

ToolChain::RuntimeLibType Kot::GetRuntimeLibType(const ArgList &Args) const{
  if(Arg *A = Args.getLastArg(clang::driver::options::OPT_rtlib_EQ)){
    StringRef Value = A->getValue();
    if(Value != "compiler-rt"){
      getDriver().Diag(clang::diag::err_drv_invalid_rtlib_name) << A->getAsString(Args);
    }
  }
  return ToolChain::RLT_CompilerRT;
}

ToolChain::UnwindLibType Kot::GetUnwindLibType(const ArgList &Args) const{
  return ToolChain::UNW_None;
}

ToolChain::CXXStdlibType Kot::GetCXXStdlibType(const ArgList &Args) const{
  if(Arg *A = Args.getLastArg(options::OPT_stdlib_EQ)){
    StringRef Value = A->getValue();
    if(Value != "libc++"){
      getDriver().Diag(diag::err_drv_invalid_stdlib_name) << A->getAsString(Args);
    }
  }
  return ToolChain::CST_Libcxx;
}

void Kot::addClangTargetOptions(const ArgList &DriverArgs, ArgStringList &CC1Args, Action::OffloadKind) const{
  if(!DriverArgs.hasFlag(options::OPT_fuse_init_array, options::OPT_fno_use_init_array, true)){
    CC1Args.push_back("-fno-use-init-array");
  }

  CC1Args.push_back("-mlong-double-64");

  CC1Args.push_back("-ffunction-sections");
  CC1Args.push_back("-fdata-sections");
  CC1Args.push_back("-fno-rtti");
}

void Kot::AddClangSystemIncludeArgs(const ArgList &DriverArgs, ArgStringList &CC1Args) const{
  const Driver &D = getDriver();

  if(DriverArgs.hasArg(options::OPT_nostdinc)){
    return;
  }

  if(!DriverArgs.hasArg(options::OPT_nobuiltininc)){
    SmallString<128> P(D.ResourceDir);
    llvm::sys::path::append(P, "include");
    addSystemInclude(DriverArgs, CC1Args, P);
  }

  if(DriverArgs.hasArg(options::OPT_nostdlibinc)){
    return;
  }

  if (!D.SysRoot.empty()) {
    SmallString<128> P(D.SysRoot);
    llvm::sys::path::append(P, "include");
    addExternCSystemInclude(DriverArgs, CC1Args, P.str());
  }
}

void Kot::AddClangCXXStdlibIncludeArgs(const ArgList &DriverArgs, ArgStringList &CC1Args) const{
  if(DriverArgs.hasArg(options::OPT_nostdlibinc) || DriverArgs.hasArg(options::OPT_nostdincxx)){
    return;
  }

  switch(GetCXXStdlibType(DriverArgs)){
    case ToolChain::CST_Libcxx:{
      SmallString<128> P(getDriver().SysRoot);
      llvm::sys::path::append(P, "include", "c++", "v1");
      addSystemInclude(DriverArgs, CC1Args, P.str());
      break;
    }
    default:{
      llvm_unreachable("invalid stdlib name");
    }
  }
}

void Kot::AddCXXStdlibLibArgs(const ArgList &Args, ArgStringList &CmdArgs) const{
  switch(GetCXXStdlibType(Args)){
    case ToolChain::CST_Libcxx:{
      CmdArgs.push_back("-lc++");
      break;
    }
    case ToolChain::CST_Libstdcxx:{
      llvm_unreachable("invalid stdlib name");
    }
  }
}