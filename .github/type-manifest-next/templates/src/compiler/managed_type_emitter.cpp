#include "managed_type_emitter.hpp"

#include "cvite/managed_type.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace cvite::compiler {
namespace {

struct Hash128 final {
    std::uint64_t high = 0U;
    std::uint64_t low = 0U;
    std::string hex;
};

Hash128 hash128(llvm::StringRef input)
{
    llvm::MD5 hash;
    llvm::MD5::MD5Result result;
    llvm::SmallString<32> text;
    hash.update(input);
    hash.final(result);
    llvm::MD5::stringifyResult(result, text);
    const auto words = result.words();
    return {words.first, words.second, text.str().str()};
}

std::string layoutSeed(const cvite::semantic::RecordInfo &record)
{
    std::ostringstream output;
    output << "cvite.managed-layout.v1\n";
    output << "id=" << record.id << '\n';
    output << "size=" << record.size << '\n';
    output << "alignment=" << record.alignment << '\n';
    output << "union=" << (record.is_union ? 1 : 0) << '\n';
    for (const cvite::semantic::FieldInfo &field : record.fields) {
        output << "field=" << field.id << '\n';
        output << "name=" << field.name << '\n';
        output << "type=" << field.canonical_type << '\n';
        output << "offset=" << field.bit_offset << '\n';
        output << "size=" << field.bit_size << '\n';
        output << "alignment=" << field.alignment << '\n';
        output << "bit-width=" << field.bit_width << '\n';
        output << "bit-field=" << (field.bit_field ? 1 : 0) << '\n';
        output << "flexible=" << (field.flexible_array ? 1 : 0) << '\n';
    }
    return output.str();
}

llvm::GlobalVariable *stringGlobal(
    llvm::Module &module,
    llvm::StringRef prefix,
    llvm::StringRef text,
    llvm::StringRef suffix)
{
    llvm::Constant *data = llvm::ConstantDataArray::getString(
        module.getContext(), text, true);
    auto *global = new llvm::GlobalVariable(
        module,
        data->getType(),
        true,
        llvm::GlobalValue::PrivateLinkage,
        data,
        prefix.str() + suffix.str());
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    global->setAlignment(llvm::Align(1U));
    return global;
}

std::uint64_t fieldFlags(const cvite::semantic::FieldInfo &field)
{
    std::uint64_t flags = 0U;
    if (field.bit_field) {
        flags |= CVITE_MANAGED_FIELD_BIT_FIELD;
    }
    if (field.flexible_array) {
        flags |= CVITE_MANAGED_FIELD_FLEXIBLE_ARRAY;
    }
    if (field.bit_offset < 0 || field.bit_size <= 0 ||
        (field.bit_offset % 8) != 0 || (field.bit_size % 8) != 0) {
        flags |= CVITE_MANAGED_FIELD_NON_BYTE_ADDRESSABLE;
    }
    return flags;
}

std::uint64_t byteOffset(const cvite::semantic::FieldInfo &field)
{
    return field.bit_offset < 0 || (field.bit_offset % 8) != 0
        ? 0U
        : static_cast<std::uint64_t>(field.bit_offset / 8);
}

std::uint64_t byteSize(const cvite::semantic::FieldInfo &field)
{
    return field.bit_size <= 0 || (field.bit_size % 8) != 0
        ? 0U
        : static_cast<std::uint64_t>(field.bit_size / 8);
}

struct EmittedType final {
    const cvite::semantic::RecordInfo *record = nullptr;
    Hash128 identity;
    Hash128 layout;
    llvm::GlobalVariable *fields = nullptr;
    llvm::GlobalVariable *name = nullptr;
};

} // namespace

bool emitManagedTypeManifest(
    llvm::Module &module,
    const cvite::semantic::AllocationIndex &allocations,
    const cvite::semantic::SemanticIndex &semantics,
    std::string &error)
{
    if (module.getNamedGlobal(CVITE_MANAGED_TYPE_MANIFEST_SYMBOL) != nullptr) {
        error = "managed type manifest already exists";
        return false;
    }

    std::map<std::string, const cvite::semantic::RecordInfo *> selected;
    for (const cvite::semantic::AllocationSite &site : allocations.sites) {
        if (site.confidence ==
                cvite::semantic::AllocationConfidence::unknown ||
            site.type_id.empty()) {
            continue;
        }
        const cvite::semantic::RecordInfo *record =
            cvite::semantic::findRecord(semantics, site.type_id);
        if (record == nullptr) {
            record = cvite::semantic::findRecord(semantics, site.type_name);
        }
        if (record == nullptr) {
            error = "typed allocation record is absent from semantic index: " +
                site.type_name;
            return false;
        }
        selected[record->id] = record;
    }

    llvm::LLVMContext &context = module.getContext();
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    llvm::Type *pointer = llvm::PointerType::getUnqual(context);
    llvm::StructType *field_type = llvm::StructType::get(
        context,
        {i64, i64, i64, i64, i64, i64, i64, pointer},
        false);
    llvm::StructType *type_type = llvm::StructType::get(
        context,
        {i64, i64, i64, i64, i64, i64, i64, pointer, pointer},
        false);
    llvm::StructType *manifest_type = llvm::StructType::get(
        context, {i64, i64, pointer}, false);

    std::vector<EmittedType> emitted;
    emitted.reserve(selected.size());
    for (const auto &entry : selected) {
        const cvite::semantic::RecordInfo &record = *entry.second;
        if (record.size < 0 || record.alignment <= 0) {
            error = "managed record has incomplete layout: " + record.name;
            return false;
        }
        EmittedType type;
        type.record = &record;
        type.identity = hash128(record.id);
        type.layout = hash128(layoutSeed(record));
        type.name = stringGlobal(
            module,
            "__cvite_managed_type_name.",
            record.name,
            type.identity.hex);

        std::vector<llvm::Constant *> field_values;
        field_values.reserve(record.fields.size());
        for (const cvite::semantic::FieldInfo &field : record.fields) {
            const Hash128 field_identity = hash128(field.id);
            const Hash128 field_type_identity = hash128(field.canonical_type);
            llvm::GlobalVariable *field_name = stringGlobal(
                module,
                "__cvite_managed_field_name.",
                field.name,
                type.identity.hex + "." + field_identity.hex);
            field_values.push_back(llvm::ConstantStruct::get(
                field_type,
                {
                    llvm::ConstantInt::get(i64, field_identity.high),
                    llvm::ConstantInt::get(i64, field_identity.low),
                    llvm::ConstantInt::get(i64, field_type_identity.high),
                    llvm::ConstantInt::get(i64, field_type_identity.low),
                    llvm::ConstantInt::get(i64, byteOffset(field)),
                    llvm::ConstantInt::get(i64, byteSize(field)),
                    llvm::ConstantInt::get(i64, fieldFlags(field)),
                    field_name,
                }));
        }
        llvm::ArrayType *array_type = llvm::ArrayType::get(
            field_type,
            static_cast<std::uint64_t>(field_values.size()));
        type.fields = new llvm::GlobalVariable(
            module,
            array_type,
            true,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(array_type, field_values),
            "__cvite_managed_fields." + type.identity.hex);
        type.fields->setAlignment(llvm::Align(8U));
        emitted.push_back(std::move(type));
    }

    std::vector<llvm::Constant *> type_values;
    type_values.reserve(emitted.size());
    for (const EmittedType &type : emitted) {
        const cvite::semantic::RecordInfo &record = *type.record;
        type_values.push_back(llvm::ConstantStruct::get(
            type_type,
            {
                llvm::ConstantInt::get(i64, type.identity.high),
                llvm::ConstantInt::get(i64, type.identity.low),
                llvm::ConstantInt::get(i64, type.layout.high),
                llvm::ConstantInt::get(i64, type.layout.low),
                llvm::ConstantInt::get(
                    i64, static_cast<std::uint64_t>(record.size)),
                llvm::ConstantInt::get(
                    i64, static_cast<std::uint64_t>(record.alignment)),
                llvm::ConstantInt::get(
                    i64, static_cast<std::uint64_t>(record.fields.size())),
                type.fields,
                type.name,
            }));
    }

    llvm::ArrayType *types_array_type = llvm::ArrayType::get(
        type_type,
        static_cast<std::uint64_t>(type_values.size()));
    auto *types_global = new llvm::GlobalVariable(
        module,
        types_array_type,
        true,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantArray::get(types_array_type, type_values),
        "__cvite_managed_types");
    types_global->setAlignment(llvm::Align(8U));

    llvm::Constant *manifest_value = llvm::ConstantStruct::get(
        manifest_type,
        {
            llvm::ConstantInt::get(
                i64, CVITE_MANAGED_TYPE_MANIFEST_SCHEMA),
            llvm::ConstantInt::get(
                i64, static_cast<std::uint64_t>(emitted.size())),
            types_global,
        });
    auto *manifest = new llvm::GlobalVariable(
        module,
        manifest_type,
        true,
        llvm::GlobalValue::ExternalLinkage,
        manifest_value,
        CVITE_MANAGED_TYPE_MANIFEST_SYMBOL);
    manifest->setAlignment(llvm::Align(8U));
    manifest->setDSOLocal(false);
    error.clear();
    return true;
}

} // namespace cvite::compiler
