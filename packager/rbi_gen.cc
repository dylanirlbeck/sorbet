#include "packager/rbi_gen.h"
#include "absl/strings/match.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/str_split.h"
#include "absl/synchronization/blocking_counter.h"
#include "common/FileOps.h"
#include "common/concurrency/ConcurrentQueue.h"
#include "common/concurrency/WorkerPool.h"
#include "core/GlobalState.h"
#include "packager/packager.h"

using namespace std;
namespace sorbet::packager {
namespace {

class Indent;

class Output final {
private:
    friend class Indent;

    fmt::memory_buffer out;
    int indent = 0;
    std::string tabStr = "";

    void resetTabString() {
        tabStr = string(indent * 2, ' ');
    }

    void tab() {
        indent++;
        resetTabString();
    }

    void untab() {
        indent--;
        resetTabString();
    }

public:
    template <typename... T> void println(fmt::format_string<T...> fmt, T &&...args) {
        fmt::format_to(std::back_inserter(out), tabStr);
        fmt::format_to(std::back_inserter(out), fmt, args...);
        fmt::format_to(std::back_inserter(out), "\n");
    }

    void println(string_view arg) {
        fmt::format_to(std::back_inserter(out), tabStr);
        // Hack: Intent even w/ multiline strings.
        string indented = absl::StrReplaceAll(arg, {{"\n", "\n" + tabStr}});
        std::copy(indented.begin(), indented.end(), std::back_inserter(out));
        fmt::format_to(std::back_inserter(out), "\n");
    }

    string toString() {
        auto output = fmt::to_string(out);
        out.clear();
        return output;
    }
};

class QuoteStringNameFormatter final {
    const core::GlobalState &gs;

public:
    QuoteStringNameFormatter(const core::GlobalState &gs) : gs(gs) {}

    void operator()(std::string *out, pair<core::ClassOrModuleRef, core::SymbolRef> klass) const {
        out->append(fmt::format("\"{}\"", klass.first.show(gs)));
    }
};

class QuoteStringFileFormatter final {
    const core::GlobalState &gs;

public:
    QuoteStringFileFormatter(const core::GlobalState &gs) : gs(gs) {}

    void operator()(std::string *out, core::FileRef file) const {
        out->append(fmt::format("\"{}\"", file.data(gs).path()));
    }
};

class Indent {
private:
    Output &out;

public:
    Indent(Output &out) : out(out) {
        out.tab();
    }
    ~Indent() {
        out.untab();
    }
};

// TODO: copied from lsp_helpers.cc. Move to a common utils package.
// TODO: Respect indentation.
core::TypePtr getResultType(const core::GlobalState &gs, const core::TypePtr &type, core::SymbolRef inWhat,
                            core::TypePtr receiver, const core::TypeConstraint *constr) {
    auto resultType = type;
    if (core::is_proxy_type(receiver)) {
        receiver = receiver.underlying(gs);
    }
    if (core::isa_type<core::AppliedType>(receiver)) {
        auto &applied = core::cast_type_nonnull<core::AppliedType>(receiver);
        /* instantiate generic classes */
        resultType =
            core::Types::resultTypeAsSeenFrom(gs, resultType, inWhat.enclosingClass(gs), applied.klass, applied.targs);
    }
    if (!resultType) {
        resultType = core::Types::untypedUntracked();
    }
    if (receiver) {
        resultType = core::Types::replaceSelfType(gs, resultType, receiver); // instantiate self types
    }
    if (constr) {
        resultType = core::Types::instantiate(gs, resultType, *constr); // instantiate generic methods
    }
    return resultType;
}

// iff a sig has more than this many parameters, then print it as a multi-line sig.
constexpr int MAX_PRETTY_SIG_ARGS = 4;
// iff a `def` would be this wide or wider, expand it to be a multi-line def.
constexpr int MAX_PRETTY_WIDTH = 80;

core::SymbolRef lookupFQN(const core::GlobalState &gs, const vector<core::NameRef> &fqn) {
    core::SymbolRef scope = core::Symbols::root();
    for (auto name : fqn) {
        if (scope.isClassOrModule()) {
            auto result = scope.asClassOrModuleRef().data(gs)->findMemberNoDealias(gs, name);
            if (!result.exists()) {
                return core::Symbols::noClassOrModule();
            }
            scope = result;
        } else {
            return core::Symbols::noClassOrModule();
        }
    }
    return scope;
}

class RBIExporter final {
private:
    const core::GlobalState &gs;
    const core::packages::PackageInfo &pkg;
    const core::ClassOrModuleRef pkgNamespace;
    const core::ClassOrModuleRef pkgTestNamespace;
    const UnorderedSet<core::ClassOrModuleRef> &pkgNamespaces;
    core::ClassOrModuleRef flatfileRecord;
    UnorderedSet<core::SymbolRef> emittedSymbols;
    // package => blame, for debugging
    UnorderedMap<core::ClassOrModuleRef, core::SymbolRef> referencedPackages;
    UnorderedSet<core::FileRef> referencedRBIs;
    vector<core::SymbolRef> toEmit;
    void maybeEmit(core::SymbolRef symbol) {
        if (symbol.isClassOrModule() && symbol.asClassOrModuleRef().data(gs)->isSingletonClass(gs)) {
            maybeEmit(symbol.asClassOrModuleRef().data(gs)->attachedClass(gs));
            return;
        }
        if (!emittedSymbols.contains(symbol) && isInPackage(symbol, symbol)) {
            emittedSymbols.insert(symbol);
            toEmit.emplace_back(symbol);
        }
    }

    bool tryEmitDefDelegator(core::MethodRef method) {
        // HACK: the loc info for the first arg goes back to a string that begins with "def_delegator" or
        // "def_delegators".
        const auto &args = method.data(gs)->arguments;
        if (args.empty()) {
            return false;
        }

        constexpr string_view defDelegator = "def_delegator"sv;
        constexpr string_view defDelegators = "def_delegators"sv;
        auto argName = args[0].argumentName(gs);
        if (!absl::StartsWith(argName, defDelegator)) {
            return false;
        }

        // This is a def_delegator. Emit it properly.
        // There are three forms:
        // def_delegator :target, :method_on_target_name
        // def_delegator :target, :method_on_target_name, :this_method_name
        // def_delegators :target, :method1_on_target_name, :method2_on_target_name, ...

        // We can emit the first two as-is. The third we can desugar into:
        // def_delegator :target, :this_method_name
        if (absl::StartsWith(argName, defDelegators)) {
            auto components = absl::StrSplit(argName, absl::ByAnyChar(" \n(),"));
            auto i = 0;
            for (auto &component : components) {
                ++i;
                if (i > 1) {
                    auto stripped = absl::StripAsciiWhitespace(component);
                    if (!stripped.empty()) {
                        out.println("def_delegator {}, :{}", stripped, method.data(gs)->name.show(gs));
                        return true;
                    }
                }
            }
            Exception::raise("Invalid def_delegator!");
        } else {
            out.println(argName);
        }
        return true;
    }

    string showType(const core::TypePtr &type) {
        if (type == nullptr) {
            return "";
        }
        enqueueSymbolsInType(type);
        return type.show(gs);
    }

    string prettySigForMethod(core::MethodRef method, const core::TypePtr &receiver, core::TypePtr retType,
                              const core::TypeConstraint *constraint) {
        ENFORCE(method.exists());
        ENFORCE(method.data(gs)->dealiasMethod(gs) == method);
        // handle this case anyways so that we don't crash in prod when this method is mis-used
        if (!method.exists()) {
            return "";
        }

        if (!retType) {
            retType = getResultType(gs, method.data(gs)->resultType, method, receiver, constraint);
        }
        string methodReturnType =
            (retType == core::Types::void_()) ? "void" : absl::StrCat("returns(", showType(retType), ")");
        vector<string> typeAndArgNames;
        vector<string> typeArguments;

        vector<string> flags;
        auto sym = method.data(gs);
        string sigCall = "sig";
        if (sym->flags.isFinal) {
            sigCall = "sig(:final)";
        }
        if (sym->flags.isAbstract) {
            flags.emplace_back("abstract");
        }
        if (sym->flags.isOverridable) {
            flags.emplace_back("overridable");
        }
        if (sym->flags.isOverride) {
            flags.emplace_back("override");
        }
        for (auto ta : method.data(gs)->typeArguments) {
            typeArguments.emplace_back(absl::StrCat(":", ta.data(gs)->name.show(gs)));
        }
        for (auto &argSym : method.data(gs)->arguments) {
            // Don't display synthetic arguments (like blk).
            if (!argSym.isSyntheticBlockArgument()) {
                auto argType = getResultType(gs, argSym.type, method, receiver, constraint);
                typeAndArgNames.emplace_back(absl::StrCat(argSym.argumentName(gs), ": ", showType(argType)));
            }
        }

        string flagString = "";
        if (!flags.empty()) {
            flagString = fmt::format("{}.", fmt::join(flags, "."));
        }
        string typeParamsString = "";
        if (!typeArguments.empty()) {
            typeParamsString = fmt::format("type_parameters({}).", fmt::join(typeArguments, ", "));
        }
        string paramsString = "";
        if (!typeAndArgNames.empty()) {
            paramsString = fmt::format("params({}).", fmt::join(typeAndArgNames, ", "));
        }

        auto oneline =
            fmt::format("{} {{{}{}{}{}}}", sigCall, flagString, typeParamsString, paramsString, methodReturnType);
        if (oneline.size() <= MAX_PRETTY_WIDTH && typeAndArgNames.size() <= MAX_PRETTY_SIG_ARGS) {
            return oneline;
        }

        if (!flags.empty()) {
            flagString = fmt::format("{}\n  .", fmt::join(flags, "\n  ."));
        }
        if (!typeArguments.empty()) {
            typeParamsString = fmt::format("type_parameters({})\n  .", fmt::join(typeArguments, ", "));
        }
        if (!typeAndArgNames.empty()) {
            paramsString = fmt::format("params(\n    {}\n  )\n  .", fmt::join(typeAndArgNames, ",\n    "));
        }
        return fmt::format("{} do\n  {}{}{}{}\nend", sigCall, flagString, typeParamsString, paramsString,
                           methodReturnType);
    }

    string prettyDefForMethod(core::MethodRef method) {
        ENFORCE(method.exists());
        // handle this case anyways so that we don't crash in prod when this method is mis-used
        if (!method.exists()) {
            return "";
        }
        auto methodData = method.data(gs);

        string visibility = "";
        if (methodData->flags.isPrivate) {
            if (methodData->owner.data(gs)->isSingletonClass(gs)) {
                visibility = "private_class_method ";
            } else {
                visibility = "private ";
            }
        } else if (methodData->flags.isProtected) {
            visibility = "protected ";
        }

        auto methodNameRef = methodData->name;
        ENFORCE(methodNameRef.exists());
        string methodName = "???";
        if (methodNameRef.exists()) {
            methodName = methodNameRef.toString(gs);
        }
        string methodNamePrefix = "";
        if (methodData->owner.exists() && methodData->owner.data(gs)->attachedClass(gs).exists()) {
            methodNamePrefix = "self.";
        }
        vector<string> prettyArgs;
        const auto &arguments = methodData->dealiasMethod(gs).data(gs)->arguments;
        ENFORCE(!arguments.empty(), "Should have at least a block arg");
        for (const auto &argSym : arguments) {
            // Don't display synthetic arguments (like blk).
            if (argSym.isSyntheticBlockArgument()) {
                continue;
            }

            string prefix = "";
            string suffix = "";
            auto argName = argSym.argumentName(gs);
            if (argName == "...") {
                prettyArgs.emplace_back(argName);
                // The remaining arguments are synthetic (<fwd-arg>, etc).
                break;
            }

            if (argSym.flags.isRepeated) {
                if (argSym.flags.isKeyword) {
                    prefix = "**"; // variadic keyword args
                } else {
                    prefix = "*"; // rest args
                }
            } else if (argSym.flags.isKeyword) {
                if (argSym.flags.isDefault) {
                    suffix = ": T.let(T.unsafe(nil), T.untyped)"; // optional keyword (has a default value)
                } else {
                    suffix = ":"; // required keyword
                }
            } else if (argSym.flags.isBlock) {
                prefix = "&";
            } else if (argSym.flags.isDefault) {
                suffix = "= T.let(T.unsafe(nil), T.untyped)";
            }

            prettyArgs.emplace_back(fmt::format("{}{}{}", prefix, argName, suffix));
        }

        string argListPrefix = "";
        string argListSeparator = "";
        string argListSuffix = "";
        if (prettyArgs.size() > 0) {
            argListPrefix = "(";
            argListSeparator = ", ";
            argListSuffix = ")";
        }

        auto result = fmt::format("{}def {}{}{}{}{}", visibility, methodNamePrefix, methodName, argListPrefix,
                                  fmt::join(prettyArgs, argListSeparator), argListSuffix);
        if (prettyArgs.size() > 0 && result.length() >= MAX_PRETTY_WIDTH) {
            argListPrefix = "(\n  ";
            argListSeparator = ",\n  ";
            argListSuffix = "\n)";
            result = fmt::format("{}def {}{}{}{}{}", visibility, methodNamePrefix, methodName, argListPrefix,
                                 fmt::join(prettyArgs, argListSeparator), argListSuffix);
        }
        return result;
    }

    void enqueueSymbolsInType(const core::TypePtr &type) {
        if (type == nullptr) {
            return;
        }
        switch (type.tag()) {
            case core::TypePtr::Tag::AliasType: {
                const auto &alias = core::cast_type_nonnull<core::AliasType>(type);
                maybeEmit(alias.symbol);
                break;
            }
            case core::TypePtr::Tag::AndType: {
                const auto &andType = core::cast_type_nonnull<core::AndType>(type);
                enqueueSymbolsInType(andType.left);
                enqueueSymbolsInType(andType.right);
                break;
            }
            case core::TypePtr::Tag::AppliedType: {
                const auto &applied = core::cast_type_nonnull<core::AppliedType>(type);
                maybeEmit(applied.klass);
                for (auto &targ : applied.targs) {
                    enqueueSymbolsInType(targ);
                }
                break;
            }
            case core::TypePtr::Tag::BlamedUntyped: {
                break;
            }
            case core::TypePtr::Tag::ClassType: {
                const auto &classType = core::cast_type_nonnull<core::ClassType>(type);
                maybeEmit(classType.symbol);
                break;
            }
            case core::TypePtr::Tag::LiteralType: {
                // No symbols here.
                break;
            }
            case core::TypePtr::Tag::MetaType: {
                const auto &metaType = core::cast_type_nonnull<core::MetaType>(type);
                enqueueSymbolsInType(metaType.wrapped);
                break;
            }
            case core::TypePtr::Tag::OrType: {
                const auto &orType = core::cast_type_nonnull<core::OrType>(type);
                enqueueSymbolsInType(orType.left);
                enqueueSymbolsInType(orType.right);
                break;
            }
            case core::TypePtr::Tag::SelfType: {
                break;
            }
            case core::TypePtr::Tag::SelfTypeParam: {
                const auto &selfTypeParam = core::cast_type_nonnull<core::SelfTypeParam>(type);
                maybeEmit(selfTypeParam.definition);
                break;
            }
            case core::TypePtr::Tag::ShapeType: {
                const auto &shapeType = core::cast_type_nonnull<core::ShapeType>(type);
                for (const auto &key : shapeType.keys) {
                    enqueueSymbolsInType(key);
                }
                for (const auto &value : shapeType.values) {
                    enqueueSymbolsInType(value);
                }
                break;
            }
            case core::TypePtr::Tag::TupleType: {
                const auto &tupleType = core::cast_type_nonnull<core::TupleType>(type);
                for (const auto &elem : tupleType.elems) {
                    enqueueSymbolsInType(elem);
                }
                break;
            }
            case core::TypePtr::Tag::TypeVar: {
                break;
            }
            case core::TypePtr::Tag::UnresolvedAppliedType: {
                const auto &unresolvedAppliedType = core::cast_type_nonnull<core::UnresolvedAppliedType>(type);
                maybeEmit(unresolvedAppliedType.klass);
                maybeEmit(unresolvedAppliedType.symbol);
                for (const auto &targ : unresolvedAppliedType.targs) {
                    enqueueSymbolsInType(targ);
                }
                break;
            }
            case core::TypePtr::Tag::UnresolvedClassType: {
                break;
            }
            case core::TypePtr::Tag::LambdaParam: {
                // Running .show on LambdaParam doesn't print out the types.
                // const auto &lambdaParam = core::cast_type_nonnull<core::LambdaParam>(type);
                // enqueueSymbolsInType(lambdaParam.lowerBound);
                // enqueueSymbolsInType(lambdaParam.upperBound);
                break;
            }
        }
    }

    Output out;

    // copied from variance.cc
    string showVariance(core::TypeMemberRef tm) {
        if (tm.data(gs)->isFixed()) {
            auto &lambdaParam = core::cast_type_nonnull<core::LambdaParam>(tm.data(gs)->resultType);
            return absl::StrCat("fixed: ", showType(lambdaParam.upperBound));
        }

        switch (tm.data(gs)->variance()) {
            case core::Variance::CoVariant:
                return ":out";
            case core::Variance::Invariant:
                // Default.
                return "";
            case core::Variance::ContraVariant:
                return ":in";
        }
    }

    bool isInTestPackage(core::SymbolRef sym) {
        if (sym == core::Symbols::root() || sym == core::Symbols::PackageRegistry()) {
            return false;
        }
        if (sym == pkgNamespace) {
            return false;
        }
        if (sym == pkgTestNamespace) {
            return true;
        }
        if (sym.isClassOrModule()) {
            if (pkgNamespaces.contains(sym.asClassOrModuleRef())) {
                return false;
            }
        }
        return isInTestPackage(sym.owner(gs));
    }

    bool isInPackage(core::SymbolRef sym, core::SymbolRef original) {
        if (sym == core::Symbols::root() || sym == core::Symbols::PackageRegistry()) {
            // Symbol isn't part of a package. Check if it was defined in an RBI.
            auto loc = original.loc(gs);
            if (loc.exists() && loc.file().data(gs).isRBI() && !loc.file().data(gs).isPayload()) {
                referencedRBIs.insert(loc.file());
            }
            return false;
        }
        if (sym == pkgNamespace || sym == pkgTestNamespace) {
            return true;
        }
        if (sym.isClassOrModule()) {
            if (pkgNamespaces.contains(sym.asClassOrModuleRef())) {
                referencedPackages[sym.asClassOrModuleRef()] = original;
                return false;
            }
        }
        return isInPackage(sym.owner(gs), original);
    }

    string typeDeclaration(const core::TypePtr &type) {
        if (type == nullptr) {
            return absl::StrCat("T.let(T.unsafe(nil), ", core::Types::untypedUntracked().show(gs), ")");
        } else if (core::isa_type<core::AliasType>(type)) {
            auto alias = core::cast_type_nonnull<core::AliasType>(type);
            maybeEmit(alias.symbol);
            return alias.symbol.show(gs);
        } else {
            return absl::StrCat("T.let(T.unsafe(nil), ", showType(type), ")");
        }
    }

    bool shouldSkipMember(core::NameRef name) {
        if (name.kind() == core::NameKind::UNIQUE) {
            return true;
        }

        return name == core::Names::singleton() || name == core::Names::Constants::AttachedClass() ||
               name == core::Names::attached();
    }

    void emitProp(core::NameRef name, const core::TypePtr &type, bool isConst, bool hasDefault) {
        string_view propType = isConst ? "const"sv : "prop"sv;
        out.println("{} :{}, {}{}", propType, name.show(gs), showType(type),
                    hasDefault ? absl::StrCat(", default: T.let(T.unsafe(nil), ", showType(type), ")") : "");
    }

    void removePropField(vector<core::FieldRef> &fields, core::NameRef name) {
        auto fieldName = name.lookupWithAt(gs);
        fields.erase(
            remove_if(fields.begin(), fields.end(),
                      [&gs = this->gs, fieldName](core::FieldRef field) { return field.data(gs)->name == fieldName; }),
            fields.end());
    }

    void removePropMethods(vector<core::MethodRef> &methods, core::NameRef name, bool &isConst) {
        auto equalName = name.lookupWithEq(gs);
        // Remove methods with the given name or `name=`.
        // This isn't very efficient but YOLO.
        methods.erase(remove_if(methods.begin(), methods.end(),
                                [name, equalName, &gs = this->gs, &isConst](core::MethodRef method) {
                                    auto methodName = method.data(gs)->name;
                                    if (methodName == equalName) {
                                        isConst = false;
                                        return true;
                                    }
                                    return methodName == name;
                                }),
                      methods.end());
    }

    void maybeEmitStructProps(core::MethodRef structInitializer, vector<core::MethodRef> methods,
                              vector<core::FieldRef> fields) {
        for (auto &arg : structInitializer.data(gs)->arguments) {
            if (arg.isSyntheticBlockArgument() || !arg.flags.isKeyword) {
                continue;
            }
            bool hasDefault = arg.flags.isDefault;
            bool isConst = true;
            auto name = arg.name;

            removePropMethods(methods, name, isConst);
            removePropField(fields, name);
            emitProp(name, arg.type, isConst, hasDefault);
        }

        // If the user wrote their own initializer, it will unfortunately be marked as rewriter synthesized.
        // rewriter-created initializers use the loc of the class, whereas user initializers have their own loc.
        // Use that to determine if we should emit an initialize block.
        // This is important if a class has custom @fields that are non-nilable and thus need to be defined inside
        // `initialize`.
        if (structInitializer.data(gs)->loc() != structInitializer.data(gs)->owner.data(gs)->loc()) {
            emit(structInitializer, fields);
        }

        // Emit the rest of the methods and fields.
        for (auto method : methods) {
            emit(method, fields);
        }
    }

    bool isPropMethod(core::MethodRef method) {
        if (absl::EndsWith(method.data(gs)->name.shortName(gs), "=")) {
            // If there is a prop= method, there will be a prop method.
            return false;
        }

        auto src = method.data(gs)->loc().source(gs);
        if (!src) {
            return false;
        }
        return absl::StartsWith(*src, "prop ") || absl::StartsWith(*src, "const ");
    }

    bool isFlatfileFieldMethod(core::MethodRef method) {
        if (absl::EndsWith(method.data(gs)->name.shortName(gs), "=")) {
            // If there is a prop= method, there will be a prop method.
            return false;
        }

        auto src = method.data(gs)->loc().source(gs);
        if (!src) {
            return false;
        }
        return absl::StartsWith(*src, "field ") || absl::StartsWith(*src, "from ") ||
               absl::StartsWith(*src, "pattern ") || absl::StartsWith(*src, "pattern(");
    }

    bool isFlatfile(core::ClassOrModuleRef klass) {
        if (klass == core::Symbols::root() || !klass.exists()) {
            return false;
        }
        if (klass == flatfileRecord) {
            return true;
        }
        return isFlatfile(klass.data(gs)->superClass());
    }

    void emit(core::ClassOrModuleRef klass) {
        if (!isInPackage(klass, klass) || !emittedSymbols.contains(klass)) {
            // We don't emit class definitions for items defined in other packages.
            Exception::raise("Invalid klass");
        }

        if (klass.data(gs)->superClass().data(gs)->superClass() == core::Symbols::T_Enum()) {
            // Enum value class created in TEnum rewriter pass.
            return;
        }

        if (absl::StartsWith(klass.data(gs)->name.shortName(gs), "<")) {
            // Sorbet-internal class (e.g., a test suite (`describe`)).
            return;
        }

        const bool isEnum = klass.data(gs)->superClass() == core::Symbols::T_Enum();
        const bool isStruct = klass.data(gs)->superClass() == core::Symbols::T_Struct();
        const bool isFlatFile = this->isFlatfile(klass);

        // cerr << "Emitting " << klass.show(gs) << "\n";
        // Class definition line
        auto defType = klass.data(gs)->isClassOrModuleClass() ? "class" : "module";
        auto fullName = klass.show(gs);
        string superClassString;
        if (klass.data(gs)->superClass().exists()) {
            auto superClass = klass.data(gs)->superClass();
            if (superClass != core::Symbols::Sorbet_Private_Static_ImplicitModuleSuperClass()) {
                maybeEmit(superClass);
                superClassString = absl::StrCat(" < ", superClass.show(gs));
            }
        }
        out.println("{} {}{}", defType, fullName, superClassString);

        {
            Indent indent(out);

            if (klass.data(gs)->isClassOrModuleAbstract()) {
                out.println("abstract!");
            }

            if (klass.data(gs)->isClassOrModuleFinal()) {
                out.println("final!");
            }

            if (klass.data(gs)->isClassOrModuleInterface()) {
                out.println("interface!");
            }

            if (klass.data(gs)->isClassOrModuleSealed()) {
                out.println("sealed!");
            }

            // Mixins (include/extend)
            for (auto mixin : klass.data(gs)->mixins()) {
                auto isSingleton = mixin.data(gs)->isSingletonClass(gs);
                auto keyword = isSingleton ? "extend"sv : "include"sv;
                out.println("{} {}", keyword, mixin.show(gs));
                maybeEmit(mixin);
            }

            // Type members
            for (auto typeMember : klass.data(gs)->typeMembers()) {
                emit(typeMember);
            }

            // Members
            core::MethodRef initializeMethod;
            vector<core::FieldRef> pendingFields;
            vector<core::ClassOrModuleRef> pendingEnumValues;
            vector<core::MethodRef> pendingMethods;
            for (auto &[name, member] : klass.data(gs)->membersStableOrderSlow(gs)) {
                if (shouldSkipMember(name)) {
                    continue;
                }

                switch (member.kind()) {
                    case core::SymbolRef::Kind::ClassOrModule: {
                        auto memberKlass = member.asClassOrModuleRef();
                        if (pkgNamespaces.contains(memberKlass)) {
                            // Ignore members of this class/module that are subpackages.
                            // Fixes issues where .deps.json contains subpackages despite there being no references to
                            // subpackages in the .rbi.
                            continue;
                        }
                        if (isEnum && memberKlass.data(gs)->superClass() == klass) {
                            pendingEnumValues.emplace_back(memberKlass);
                        } else {
                            // Emit later.
                            maybeEmit(member);
                        }
                        break;
                    }
                    case core::SymbolRef::Kind::TypeMember: {
                        // Ignore; already emitted above.
                        break;
                    }
                    case core::SymbolRef::Kind::TypeArgument: {
                        ENFORCE(false, "classes should never contain type arguments");
                        break;
                    }
                    case core::SymbolRef::Kind::Method: {
                        if (name == core::Names::initialize()) {
                            // Defer outputting until we gather fields.
                            initializeMethod = member.asMethodRef();
                        } else {
                            pendingMethods.emplace_back(member.asMethodRef());
                        }
                        break;
                    }
                    case core::SymbolRef::Kind::FieldOrStaticField: {
                        auto field = member.asFieldRef();
                        if (field.data(gs)->flags.isField) {
                            pendingFields.emplace_back(field);
                        } else {
                            if (absl::StartsWith(field.data(gs)->name.show(gs), "@@")) {
                                emit(field, true);
                            } else {
                                maybeEmit(field);
                            }
                        }
                        break;
                    }
                }
            }

            if (isStruct) {
                // T::Struct is special because the presence of default prop values changes
                // the initializer that Props.cc creates. We use the initialize method to
                // determine which props have default values.
                maybeEmitStructProps(initializeMethod, move(pendingMethods), move(pendingFields));
            } else if (initializeMethod.exists()) {
                emit(initializeMethod, pendingFields);
            }

            // Need to check for props and remove any fields that match them.
            {
                vector<core::MethodRef> propMethods;
                // Done in two phases to prevent mutating `pendingMethods` in loop body.
                for (auto method : pendingMethods) {
                    if (isPropMethod(method)) {
                        propMethods.emplace_back(method);
                    }
                }

                for (auto &propMethod : propMethods) {
                    auto name = propMethod.data(gs)->name;
                    removePropField(pendingFields, name);
                    bool isConst = true;
                    removePropMethods(pendingMethods, name, isConst);
                    // Defaults are not semantically important on non-T-Struct props.
                    bool hasDefault = false;
                    emitProp(name, propMethod.data(gs)->resultType, isConst, hasDefault);
                }
            }

            if (isFlatFile) {
                // Need to check for flatfile fields and emit them specially.
                vector<core::MethodRef> fieldMethods;
                for (auto method : pendingMethods) {
                    if (isFlatfileFieldMethod(method)) {
                        fieldMethods.emplace_back(method);
                    }
                }

                if (!fieldMethods.empty()) {
                    out.println("flatfile do");
                    {
                        Indent indent(out);
                        for (auto &fieldMethod : fieldMethods) {
                            auto name = fieldMethod.data(gs)->name;
                            bool isConst = true;
                            // need the same logic for flatfile fields :shrug:
                            removePropMethods(pendingMethods, name, isConst);
                            // sorbet doesn't care about if it's field/pattern/etc; it just needs the name.
                            out.println("field :{}", name.show(gs));
                        }
                    }
                    out.println("end");
                }
            }

            for (auto method : pendingMethods) {
                emit(method, pendingFields);
            }
            pendingMethods.clear();

            auto singleton = klass.data(gs)->lookupSingletonClass(gs);
            if (singleton.exists()) {
                // Mixins (include/extend)
                for (auto mixin : singleton.data(gs)->mixins()) {
                    out.println("extend {}", mixin.show(gs));
                    maybeEmit(mixin);
                }

                // Type templates
                for (auto typeMember : singleton.data(gs)->typeMembers()) {
                    emit(typeMember);
                }

                for (auto &[name, member] : singleton.data(gs)->membersStableOrderSlow(gs)) {
                    if (shouldSkipMember(name)) {
                        continue;
                    }

                    switch (member.kind()) {
                        case core::SymbolRef::Kind::ClassOrModule: {
                            if (pkgNamespaces.contains(member.asClassOrModuleRef())) {
                                // Ignore members of this class/module that are subpackages.
                                // Fixes issues where .deps.json contains subpackages despite there being no references
                                // to subpackages in the .rbi.
                                continue;
                            }
                            maybeEmit(member);
                            break;
                        }
                        case core::SymbolRef::Kind::TypeMember: {
                            // Ignore; already emitted above.
                            break;
                        }
                        case core::SymbolRef::Kind::TypeArgument: {
                            ENFORCE(false, "classes should never contain type arguments");
                            break;
                        }
                        case core::SymbolRef::Kind::Method: {
                            if (klass.data(gs)->isClassOrModuleSealed() && name == core::Names::sealedSubclasses()) {
                                // Ignore: Generated by TEnum rewriter pass.
                            } else {
                                emit(member.asMethodRef(), pendingFields);
                            }
                            break;
                        }
                        case core::SymbolRef::Kind::FieldOrStaticField: {
                            auto field = member.asFieldRef();
                            if (field.data(gs)->flags.isField) {
                                emit(field);
                            } else {
                                if (absl::StartsWith(field.data(gs)->name.show(gs), "@@")) {
                                    emit(field, true);
                                } else {
                                    maybeEmit(field);
                                }
                            }
                            break;
                        }
                    }
                }
            }

            if (isEnum && !pendingEnumValues.empty()) {
                out.println("enums do");
                {
                    Indent indentEnumBlock(out);
                    for (auto enumVal : pendingEnumValues) {
                        out.println("{} = new", enumVal.data(gs)->name.show(gs));
                    }
                }
                out.println("end");
            }
        }

        out.println("end");
    }

    // Emits method and declares fields in its body.
    void emit(core::MethodRef method, vector<core::FieldRef> &fields) {
        if (emittedSymbols.contains(method)) {
            return;
        }

        if (method.data(gs)->name == core::Names::staticInit()) {
            return;
        }
        emittedSymbols.insert(method);

        // Note: We have to emit private methods because `include`ing a module with private methods will make those
        // methods available.

        if (absl::StartsWith(method.data(gs)->name.shortName(gs), "<")) {
            // Sorbet-internal method (e.g., a test method).
            if (method.data(gs)->name == core::Names::mixedInClassMethods()) {
                auto &mixedIn = (core::cast_type<core::TupleType>(method.data(gs)->resultType))->elems;
                for (auto &mixedType : mixedIn) {
                    auto mixed = core::cast_type_nonnull<core::ClassType>(mixedType);
                    out.println("mixes_in_class_methods({})", mixed.show(gs));
                }
            }
            return;
        }

        if (tryEmitDefDelegator(method)) {
            return;
        }

        // cerr << "Emitting " << method.show(gs) << "\n";

        if (method.data(gs)->hasSig()) {
            auto dealiasedMethod = method.data(gs)->dealiasMethod(gs);
            out.println(prettySigForMethod(dealiasedMethod, nullptr, dealiasedMethod.data(gs)->resultType, nullptr));
        }
        if (fields.empty() || method.data(gs)->flags.isAbstract) {
            out.println(prettyDefForMethod(method) + "; end");
        } else {
            out.println(prettyDefForMethod(method));
            {
                Indent indent(out);
                for (auto field : fields) {
                    emit(field);
                }
                fields.clear();
            }
            out.println("end");
        }
    }

    void emit(core::FieldRef field, bool isCVar = false) {
        // cerr << "Emitting " << field.show(gs) << "\n";
        if (field.data(gs)->flags.isStaticField) {
            const auto &resultType = field.data(gs)->resultType;
            if (resultType != nullptr) {
                if (core::isa_type<core::AliasType>(resultType)) {
                    const auto &alias = core::cast_type_nonnull<core::AliasType>(resultType);
                    if (alias.symbol.isTypeMember() &&
                        alias.symbol.asTypeMemberRef().data(gs)->owner.asClassOrModuleRef().data(gs)->isSingletonClass(
                            gs)) {
                        // type_templates define static fields of the same name on the main class; ignore them.
                        return;
                    }
                } else if (core::isa_type<core::ClassType>(resultType)) {
                    auto klass = core::cast_type_nonnull<core::ClassType>(resultType).symbol;
                    if (klass.data(gs)->superClass().data(gs)->superClass() == core::Symbols::T_Enum()) {
                        // Static field defined in TEnum rewriter pass.
                        return;
                    }
                }
            }

            if (field.data(gs)->flags.isStaticFieldTypeAlias) {
                out.println("{} = T.type_alias {{{}}}", field.show(gs), showType(resultType));
            } else if (isCVar) {
                out.println("{} = {}", field.data(gs)->name.show(gs), typeDeclaration(resultType));
            } else {
                out.println("{} = {}", field.show(gs), typeDeclaration(resultType));
            }
        } else {
            out.println("{} = {}", field.data(gs)->name.show(gs), typeDeclaration(field.data(gs)->resultType));
        }
    }

    void emit(core::TypeMemberRef tm) {
        if (emittedSymbols.contains(tm)) {
            return;
        }
        emittedSymbols.insert(tm);

        // cerr << "Emitting " << tm.show(gs) << "\n";

        if (tm.data(gs)->name == core::Names::Constants::AttachedClass()) {
            return;
        }

        // If this is a type template, there will be an alias type defined on the non-singleton class w/ the same name.
        if (tm.data(gs)->owner.asClassOrModuleRef().data(gs)->isSingletonClass(gs)) {
            out.println("{} = type_template({})", tm.data(gs)->name.show(gs), showVariance(tm));
        } else {
            out.println("{} = type_member({})", tm.data(gs)->name.show(gs), showVariance(tm));
        }
    }

    void emitLoop() {
        vector<core::FieldRef> empty;
        while (!toEmit.empty()) {
            auto symbol = toEmit.back();
            toEmit.pop_back();
            switch (symbol.kind()) {
                case core::SymbolRef::Kind::ClassOrModule:
                    emit(symbol.asClassOrModuleRef());
                    break;
                case core::SymbolRef::Kind::Method:
                    emit(symbol.asMethodRef(), empty);
                    break;
                case core::SymbolRef::Kind::FieldOrStaticField:
                    emit(symbol.asFieldRef());
                    break;
                case core::SymbolRef::Kind::TypeMember:
                    break;
                case core::SymbolRef::Kind::TypeArgument:
                    break;
            }
        }
    }

    static core::ClassOrModuleRef getPkgTestNamespace(const core::GlobalState &gs,
                                                      const core::packages::PackageInfo &pkg) {
        vector<core::NameRef> fullName = pkg.fullName();
        fullName.insert(fullName.begin(), core::Names::Constants::Test());
        return lookupFQN(gs, fullName).asClassOrModuleRef();
    }

public:
    RBIExporter(const core::GlobalState &gs, const core::packages::PackageInfo &pkg,
                const UnorderedSet<core::ClassOrModuleRef> &pkgNamespaces)
        : gs(gs), pkg(pkg), pkgNamespace(lookupFQN(gs, pkg.fullName()).asClassOrModuleRef()),
          pkgTestNamespace(getPkgTestNamespace(gs, pkg)), pkgNamespaces(pkgNamespaces) {
        const auto flatFiles = gs.lookupClassSymbol(core::Symbols::Opus(), gs.lookupNameConstant("Flatfiles"));
        if (flatFiles.exists()) {
            flatfileRecord = gs.lookupClassSymbol(flatFiles, gs.lookupNameConstant("Record"));
        }
    }

    RBIGenerator::RBIOutput emit() {
        RBIGenerator::RBIOutput output;
        output.baseFilePath = pkg.mangledName().show(gs);

        auto rawExports = pkg.exports();
        auto rawTestExports = pkg.testExports();

        vector<core::SymbolRef> exports;
        vector<core::SymbolRef> testExports;

        for (auto &e : rawExports) {
            auto exportSymbol = lookupFQN(gs, e);
            if (exportSymbol.exists()) {
                if (isInTestPackage(exportSymbol)) {
                    // Test:: symbol.
                    testExports.emplace_back(exportSymbol);
                } else {
                    exports.emplace_back(exportSymbol);
                }
            }
        }

        for (auto e : rawTestExports) {
            auto exportSymbol = lookupFQN(gs, e);
            if (exportSymbol.exists()) {
                testExports.emplace_back(exportSymbol);
            }
        }

        if (!exports.empty()) {
            for (auto &exportSymbol : exports) {
                maybeEmit(exportSymbol);
            }

            emitLoop();

            output.rbi = "# typed: true\n\n" + out.toString();
            output.rbiPackageDependencies = fmt::format(
                "{{\"packageRefs\":[{}], \"rbiRefs\":[{}]}}",
                absl::StrJoin(referencedPackages.begin(), referencedPackages.end(), ",", QuoteStringNameFormatter(gs)),
                absl::StrJoin(referencedRBIs.begin(), referencedRBIs.end(), ",", QuoteStringFileFormatter(gs)));
        }

        // N.B.: We don't need to generate this in the same pass. Test code only relies on exported symbols from regular
        // code...
        if (!testExports.empty()) {
            for (auto exportSymbol : testExports) {
                maybeEmit(exportSymbol);
            }

            emitLoop();

            auto rbiText = out.toString();
            if (!rbiText.empty()) {
                output.testRBI = "# typed: true\n\n" + rbiText;
                output.testRBIPackageDependencies = fmt::format(
                    "{{\"packageRefs\":[{}], \"rbiRefs\":[{}]}}",
                    absl::StrJoin(referencedPackages.begin(), referencedPackages.end(), ",",
                                  QuoteStringNameFormatter(gs)),
                    absl::StrJoin(referencedRBIs.begin(), referencedRBIs.end(), ",", QuoteStringFileFormatter(gs)));
            }
        }

        return output;
    }
};
} // namespace

UnorderedSet<core::ClassOrModuleRef>
RBIGenerator::buildPackageNamespace(core::GlobalState &gs, vector<ast::ParsedFile> &packageFiles, WorkerPool &workers) {
    // Populate package database.
    packageFiles = Packager::findPackages(gs, workers, move(packageFiles));

    const auto &packageDB = gs.packageDB();

    auto &packages = packageDB.packages();
    if (packages.empty()) {
        Exception::raise("No packages found?");
    }

    auto testNamespace = core::Names::Constants::Test();

    UnorderedSet<core::ClassOrModuleRef> packageNamespaces;
    for (auto package : packages) {
        auto &pkg = gs.packageDB().getPackageInfo(package);
        vector<core::NameRef> fullName = pkg.fullName();
        auto packageNamespace = lookupFQN(gs, fullName);
        // Might not exist if package has no files.
        if (packageNamespace.exists()) {
            packageNamespaces.insert(packageNamespace.asClassOrModuleRef());
        }

        fullName.insert(fullName.begin(), testNamespace);
        auto testPackageNamespace = lookupFQN(gs, fullName);
        if (testPackageNamespace.exists()) {
            packageNamespaces.insert(testPackageNamespace.asClassOrModuleRef());
        }
    }

    return packageNamespaces;
}

RBIGenerator::RBIOutput RBIGenerator::runOnce(const core::GlobalState &gs, core::NameRef pkgName,
                                              const UnorderedSet<core::ClassOrModuleRef> &packageNamespaces) {
    auto &pkg = gs.packageDB().getPackageInfo(pkgName);
    ENFORCE(pkg.exists());
    RBIExporter exporter(gs, pkg, packageNamespaces);
    return exporter.emit();
}

void RBIGenerator::run(core::GlobalState &gs, vector<ast::ParsedFile> packageFiles, string outputDir,
                       WorkerPool &workers) {
    absl::BlockingCounter threadBarrier(std::max(workers.size(), 1));
    UnorderedSet<core::ClassOrModuleRef> packageNamespaces = buildPackageNamespace(gs, packageFiles, workers);

    const auto &packageDB = gs.packageDB();
    auto &packages = packageDB.packages();
    auto inputq = make_shared<ConcurrentBoundedQueue<core::NameRef>>(packages.size());
    for (auto package : packages) {
        inputq->push(move(package), 1);
    }

    const core::GlobalState &rogs = gs;
    workers.multiplexJob("RBIGenerator", [inputq, outputDir, &threadBarrier, &rogs, &packageNamespaces]() {
        core::NameRef job;
        for (auto result = inputq->try_pop(job); !result.done(); result = inputq->try_pop(job)) {
            if (result.gotItem()) {
                auto output = runOnce(rogs, job, packageNamespaces);
                if (!output.rbi.empty()) {
                    FileOps::write(absl::StrCat(outputDir, "/", output.baseFilePath, ".rbi"), output.rbi);
                    FileOps::write(absl::StrCat(outputDir, "/", output.baseFilePath, ".deps.json"),
                                   output.rbiPackageDependencies);
                }

                if (!output.testRBI.empty()) {
                    FileOps::write(absl::StrCat(outputDir, "/", output.baseFilePath, ".test.rbi"), output.testRBI);
                    FileOps::write(absl::StrCat(outputDir, "/", output.baseFilePath, ".test.deps.json"),
                                   output.testRBIPackageDependencies);
                }
            }
        }
        threadBarrier.DecrementCount();
    });
    threadBarrier.Wait();
}
} // namespace sorbet::packager