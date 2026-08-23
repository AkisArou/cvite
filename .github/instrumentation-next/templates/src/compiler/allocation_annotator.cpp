#include "cvite/allocation_index.hpp"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr llvm::StringLiteral kAllocationMetadata = "cvite.managed.alloc";

struct Hash128 final {
    std::uint64_t high = 0U;
    std::uint64_t low = 0U;
};

Hash128 hash128(llvm::StringRef value)
{
    llvm::MD5 hash;
    llvm::MD5::MD5Result result;
    hash.update(value);
    hash.final(result);
    const auto words = result.words();
    return {words.first, words.second};
}

std::string normalizePath(const std::string &path)
{
    if (path.empty()) {
        return path;
    }
    std::error_code error;
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(path, error);
    return error ? std::filesystem::path(path).lexically_normal().string()
                 : normalized.string();
}

std::string debugPath(const llvm::DILocation &location)
{
    const llvm::DIScope *scope = location.getScope();
    const llvm::DIFile *file = scope == nullptr ? nullptr : scope->getFile();
    if (file == nullptr) {
        return {};
    }
    std::filesystem::path path(file->getFilename().str());
    if (path.is_relative() && !file->getDirectory().empty()) {
        path = std::filesystem::path(file->getDirectory().str()) / path;
    }
    return normalizePath(path.string());
}

bool supportedAllocator(const llvm::Function &function)
{
    const llvm::StringRef name = function.getName();
    return name == "malloc" || name == "calloc" || name == "realloc" ||
        name == "aligned_alloc";
}

std::size_t columnDistance(unsigned left, std::uint32_t right)
{
    const std::size_t left_value = static_cast<std::size_t>(left);
    const std::size_t right_value = static_cast<std::size_t>(right);
    return left_value > right_value ? left_value - right_value
                                    : right_value - left_value;
}

const cvite::semantic::AllocationSite *findSite(
    const cvite::semantic::AllocationIndex &index,
    const llvm::DILocation &location,
    const llvm::Function &callee,
    const std::set<std::string> &used)
{
    const std::string path = debugPath(location);
    const cvite::semantic::AllocationSite *best = nullptr;
    std::size_t best_distance = std::numeric_limits<std::size_t>::max();
    for (const cvite::semantic::AllocationSite &site : index.sites) {
        if (used.find(site.id) != used.end() ||
            site.confidence ==
                cvite::semantic::AllocationConfidence::unknown ||
            site.type_id.empty() || site.line != location.getLine() ||
            normalizePath(site.source) != path ||
            cvite::semantic::allocationKindName(site.kind) !=
                callee.getName()) {
            continue;
        }
        const std::size_t distance =
            columnDistance(location.getColumn(), site.column);
        if (distance < best_distance) {
            best = &site;
            best_distance = distance;
        }
    }
    return best;
}

void attachMetadata(
    llvm::CallBase &call,
    const cvite::semantic::AllocationSite &site)
{
    llvm::LLVMContext &context = call.getContext();
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    const Hash128 identity = hash128(site.type_id);
    llvm::Metadata *operands[] = {
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(i64, identity.high)),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(i64, identity.low)),
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64, 0U)),
        llvm::MDString::get(context, site.type_id),
        llvm::MDString::get(context, site.type_name),
    };
    call.setMetadata(
        kAllocationMetadata,
        llvm::MDNode::get(context, operands));
}

void usage(const char *program)
{
    std::cerr << "usage: " << program
              << " --input module.ll --index sites.cvaidx --output annotated.ll\n";
}

bool optionValue(
    int argc,
    char **argv,
    const std::string &option,
    std::string &value)
{
    for (int index = 1; index + 1 < argc; ++index) {
        if (argv[index] == option) {
            value = argv[index + 1];
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char **argv)
{
    llvm::InitLLVM initialization(argc, argv);
    std::string input;
    std::string index_path;
    std::string output;
    if (!optionValue(argc, argv, "--input", input) ||
        !optionValue(argc, argv, "--index", index_path) ||
        !optionValue(argc, argv, "--output", output)) {
        usage(argv[0]);
        return 2;
    }

    cvite::semantic::AllocationIndex allocation_index;
    std::string error;
    if (!cvite::semantic::readAllocationIndex(
            index_path, allocation_index, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    std::unique_ptr<llvm::Module> module =
        llvm::parseIRFile(input, diagnostic, context);
    if (module == nullptr) {
        diagnostic.print(argv[0], llvm::errs());
        return 1;
    }

    std::set<std::string> used;
    std::size_t annotated = 0U;
    for (llvm::Function &function : *module) {
        if (function.isDeclaration()) {
            continue;
        }
        for (llvm::BasicBlock &block : function) {
            for (llvm::Instruction &instruction : block) {
                auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (call == nullptr || call->getDebugLoc() == nullptr) {
                    continue;
                }
                llvm::Function *callee = call->getCalledFunction();
                if (callee == nullptr || !supportedAllocator(*callee)) {
                    continue;
                }
                const cvite::semantic::AllocationSite *site = findSite(
                    allocation_index,
                    *call->getDebugLoc().get(),
                    *callee,
                    used);
                if (site == nullptr) {
                    continue;
                }
                attachMetadata(*call, *site);
                used.insert(site->id);
                ++annotated;
            }
        }
    }

    std::error_code output_error;
    llvm::raw_fd_ostream stream(output, output_error);
    if (output_error) {
        std::cerr << "cannot open annotated IR output: "
                  << output_error.message() << '\n';
        return 1;
    }
    module->print(stream, nullptr);
    stream.flush();
    if (stream.has_error()) {
        std::cerr << "failed while writing annotated IR\n";
        return 1;
    }

    std::size_t typed_sites = 0U;
    for (const auto &site : allocation_index.sites) {
        if (site.confidence !=
                cvite::semantic::AllocationConfidence::unknown &&
            !site.type_id.empty()) {
            ++typed_sites;
        }
    }
    std::cout << "annotated " << annotated << " of " << typed_sites
              << " typed allocation sites\n";
    return 0;
}
