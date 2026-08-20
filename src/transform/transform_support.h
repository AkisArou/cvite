#ifndef CVITE_TRANSFORM_SUPPORT_H
#define CVITE_TRANSFORM_SUPPORT_H

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <string>

namespace cvite::transform {

inline constexpr llvm::StringLiteral kAbiSchema = "cvite.lowered-abi.v1";
inline constexpr llvm::StringLiteral kIdentitySchema = "cvite.function-id.v1";
inline constexpr llvm::StringLiteral kFunctionMetadata = "cvite.abi";
inline constexpr llvm::StringLiteral kFunctionIndex = "cvite.functions";

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

inline std::string identitySeed(
    const llvm::Module &module,
    const llvm::Function &function,
    llvm::StringRef original_name)
{
    std::string seed;
    llvm::raw_string_ostream output(seed);

    output << kIdentitySchema << '\n';
    if (function.hasLocalLinkage()) {
        output << "scope=translation-unit\n";
        output << "source=" << module.getSourceFileName() << '\n';
    } else {
        output << "scope=linkage-unit\n";
    }
    output << "name=" << original_name << '\n';
    return output.str();
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

inline bool shouldIndex(const llvm::Function &function)
{
    return !function.isDeclaration() && !function.isIntrinsic() &&
        !function.getName().starts_with("__cvite_");
}

} // namespace cvite::transform

#endif
