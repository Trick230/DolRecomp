#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>

namespace dolllvm {

// LLVM 20 renamed Intrinsic::getDeclaration and LLVM 21 removed the old name.
inline llvm::Function *intrinsicDeclaration(llvm::Module *module,
                                            llvm::Intrinsic::ID id,
                                            llvm::ArrayRef<llvm::Type *> types = {}) {
#if LLVM_VERSION_MAJOR >= 20
  return llvm::Intrinsic::getOrInsertDeclaration(module, id, types);
#else
  return llvm::Intrinsic::getDeclaration(module, id, types);
#endif
}

} // namespace dolllvm
