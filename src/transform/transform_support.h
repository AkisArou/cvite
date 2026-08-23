#ifndef CVITE_TRANSFORM_SUPPORT_H
#define CVITE_TRANSFORM_SUPPORT_H

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalObject.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/raw_ostream.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

namespace cvite::transform {

inline constexpr llvm::StringLiteral kAbiSchema = "cvite.lowered-abi.v1";
inline constexpr llvm::StringLiteral kIdentitySchema = "cvite.function-id.v1";
inline constexpr llvm::StringLiteral kFunctionMetadata = "cvite.abi";
inline constexpr llvm::StringLiteral kFunctionIndex = "cvite.functions";
inline constexpr llvm::StringLiteral kImplementationSchema =
    "cvite.implementation.v1";
inline constexpr llvm::StringLiteral kImplementationMetadata =
    "cvite.implementation";
inline constexpr llvm::StringLiteral kOriginMetadata = "cvite.origin";
inline constexpr llvm::StringLiteral kStorageIdentitySchema =
    "cvite.storage-id.v1";
inline constexpr llvm::StringLiteral kStorageLayoutSchema =
    "cvite.storage-layout.v1";
inline constexpr llvm::StringLiteral kStorageMetadata = "cvite.storage";
inline constexpr llvm::StringLiteral kStorageSymbolPrefix =
    "__cvite_storage.";


struct DeclarationOrigin final {
    std::string source;
    std::string name;
};

inline DeclarationOrigin declarationOrigin(
    const llvm::Module &module,
    const llvm::GlobalObject &object,
    llvm::StringRef fallback_name)
{
    const llvm::MDNode *metadata = object.getMetadata(kOriginMetadata);
    if (metadata != nullptr && metadata->getNumOperands() >= 2U) {
        const auto *source = llvm::dyn_cast<llvm::MDString>(
            metadata->getOperand(0U).get());
        const auto *name = llvm::dyn_cast<llvm::MDString>(
            metadata->getOperand(1U).get());
        if (source != nullptr && name != nullptr && !name->getString().empty()) {
            return {source->getString().str(), name->getString().str()};
        }
    }

    std::string source = module.getSourceFileName();
    if (source.empty()) {
        source = module.getModuleIdentifier();
    }
    return {std::move(source), fallback_name.str()};
}

inline void setDeclarationOrigin(
    llvm::GlobalObject &object,
    llvm::StringRef source,
    llvm::StringRef name)
{
    llvm::LLVMContext &context = object.getContext();
    llvm::Metadata *values[] = {
        llvm::MDString::get(context, source),
        llvm::MDString::get(context, name),
    };
    object.setMetadata(kOriginMetadata, llvm::MDNode::get(context, values));
}

struct Hash128 final {
    std::uint64_t high = 0U;
    std::uint64_t low = 0U;
    std::string hex;
};

inline std::string printType(const llvm::Type &type)
{
    std::string text;
    llvm::raw_string_ostream output(text);
    type.print(output);
    return output.str();
}

inline void appendAttributeSet(
    llvm::raw_ostream &output,
    llvm::StringRef label,
    llvm::AttributeSet attributes)
{
    output << label << '=';
    output << attributes.getAsString();
    output << '\n';
}

inline std::string abiSeed(
    const llvm::Module &module,
    const llvm::Function &function)
{
    std::string seed;
    llvm::raw_string_ostream output(seed);
    const llvm::AttributeList attributes = function.getAttributes();

    output << kAbiSchema << '\n';
    output << "target=" << module.getTargetTriple() << '\n';
    output << "data-layout=" << module.getDataLayoutStr() << '\n';
    output << "function-type=" << printType(*function.getFunctionType()) << '\n';
    output << "calling-convention=" << function.getCallingConv() << '\n';
    appendAttributeSet(output, "return-attributes", attributes.getRetAttrs());
    for (unsigned index = 0U; index < function.arg_size(); ++index) {
        output << "parameter=" << index << '\n';
        appendAttributeSet(
            output,
            "parameter-attributes",
            attributes.getParamAttrs(index));
    }
    return output.str();
}

inline std::string implementationSeed(const llvm::Function &function)
{
    std::string seed;
    llvm::raw_string_ostream output(seed);

    /*
     * This is intentionally computed after Clang lowering but before CVite
     * renames or wraps the function. Textual LLVM IR is conservative: an
     * unrelated metadata renumbering may cause an extra publication, but a
     * semantic instruction/operand/attribute change cannot be silently
     * ignored. The 128-bit digest is a change detector, never a security
     * boundary.
     */
    output << kImplementationSchema << '\n';
    function.print(output, nullptr, false, false);
    return output.str();
}

inline std::string identitySeed(
    const llvm::Module &module,
    const llvm::Function &function,
    llvm::StringRef original_name)
{
    std::string seed;
    llvm::raw_string_ostream output(seed);
    const DeclarationOrigin origin =
        declarationOrigin(module, function, original_name);

    output << kIdentitySchema << '\n';
    if (function.hasLocalLinkage()) {
        output << "scope=translation-unit\n";
        output << "source=" << origin.source << '\n';
    } else {
        output << "scope=linkage-unit\n";
    }
    output << "name=" << origin.name << '\n';
    return output.str();
}

inline std::string storageIdentitySeed(
    const llvm::Module &module,
    const llvm::GlobalVariable &storage,
    llvm::StringRef original_name)
{
    std::string seed;
    llvm::raw_string_ostream output(seed);
    const DeclarationOrigin origin =
        declarationOrigin(module, storage, original_name);

    output << kStorageIdentitySchema << '\n';
    if (storage.hasLocalLinkage()) {
        output << "scope=translation-unit\n";
        output << "source=" << origin.source << '\n';
    } else {
        output << "scope=linkage-unit\n";
    }
    output << "name=" << origin.name << '\n';
    return output.str();
}

inline std::uint64_t storageSize(
    const llvm::Module &module,
    const llvm::GlobalVariable &storage)
{
    const llvm::TypeSize size =
        module.getDataLayout().getTypeAllocSize(storage.getValueType());
    return size.isScalable() ? 0U : size.getFixedValue();
}

inline std::uint64_t storageAlignment(
    const llvm::Module &module,
    const llvm::GlobalVariable &storage)
{
    const llvm::MaybeAlign explicit_alignment = storage.getAlign();
    if (explicit_alignment.has_value()) {
        return explicit_alignment->value();
    }
    return module.getDataLayout()
        .getABITypeAlign(storage.getValueType())
        .value();
}

inline std::string storageLayoutSeed(
    const llvm::Module &module,
    const llvm::GlobalVariable &storage)
{
    std::string seed;
    llvm::raw_string_ostream output(seed);

    output << kStorageLayoutSchema << '\n';
    output << "target=" << module.getTargetTriple() << '\n';
    output << "data-layout=" << module.getDataLayoutStr() << '\n';
    output << "value-type=" << printType(*storage.getValueType()) << '\n';
    output << "size=" << storageSize(module, storage) << '\n';
    output << "alignment=" << storageAlignment(module, storage) << '\n';
    return output.str();
}

inline std::string canonicalHex(const Hash128 &value)
{
    char output[33];
    const int length = std::snprintf(
        output,
        sizeof(output),
        "%016" PRIx64 "%016" PRIx64,
        value.high,
        value.low);
    if (length != 32) {
        return std::string();
    }
    return std::string(output, 32U);
}

inline std::string storageSymbolName(const Hash128 &identity)
{
    return kStorageSymbolPrefix.str() + canonicalHex(identity);
}

inline Hash128 hash128(llvm::StringRef input)
{
    llvm::MD5 hash;
    llvm::MD5::MD5Result result;
    llvm::SmallString<32> output;

    hash.update(input);
    hash.final(result);
    llvm::MD5::stringifyResult(result, output);
    const auto words = result.words();
    return {words.first, words.second, output.str().str()};
}

inline void setImplementationMetadata(
    llvm::Function &function,
    const Hash128 &implementation)
{
    llvm::LLVMContext &context = function.getContext();
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    llvm::Metadata *metadata[] = {
        llvm::MDString::get(context, implementation.hex),
        llvm::MDString::get(context, kImplementationSchema),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(i64, implementation.high)),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(i64, implementation.low)),
    };
    function.setMetadata(
        kImplementationMetadata,
        llvm::MDNode::get(context, metadata));
}

inline bool shouldIndex(const llvm::Function &function)
{
    return !function.isDeclaration() && !function.isIntrinsic() &&
        !function.getName().starts_with("__cvite_");
}

inline bool hasSupportedStorageLinkage(const llvm::GlobalVariable &storage)
{
    return storage.hasExternalLinkage() || storage.hasInternalLinkage() ||
        storage.hasPrivateLinkage() || storage.hasCommonLinkage();
}

inline bool shouldTrackStorageDefinition(
    const llvm::GlobalVariable &storage)
{
    return !storage.isDeclaration() && !storage.isConstant() &&
        !storage.isThreadLocal() && storage.getAddressSpace() == 0U &&
        storage.getValueType()->isSized() &&
        hasSupportedStorageLinkage(storage) &&
        !storage.getName().starts_with("llvm.") &&
        !storage.getName().starts_with("__cvite_");
}

} // namespace cvite::transform

#endif
