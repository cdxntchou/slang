// slang-emit-c-like.cpp
#include "slang-emit-c-like.h"

#include "../core/slang-writer.h"
#include "slang-ir-bind-existentials.h"
#include "slang-ir-dce.h"
#include "slang-ir-entry-point-uniforms.h"
#include "slang-ir-glsl-legalize.h"

#include "slang-ir-link.h"
#include "slang-ir-restructure-scoping.h"
#include "slang-ir-specialize.h"
#include "slang-ir-specialize-resources.h"
#include "slang-ir-ssa.h"
#include "slang-ir-union.h"
#include "slang-ir-validate.h"
#include "slang-legalize-types.h"
#include "slang-lower-to-ir.h"
#include "slang-mangle.h"
#include "slang-name.h"
#include "slang-syntax.h"
#include "slang-type-layout.h"
#include "slang-visitor.h"

#include "slang-emit-source-writer.h"
#include "slang-mangled-lexer.h"

#include <assert.h>

namespace Slang {

// represents a declarator for use in emitting types
struct CLikeSourceEmitter::EDeclarator
{
    enum class Flavor
    {
        name,
        Array,
        UnsizedArray,
    };
    Flavor flavor;
    EDeclarator* next = nullptr;

    // Used for `Flavor::name`
    Name*       name;
    SourceLoc   loc;

    // Used for `Flavor::Array`
    IRInst* elementCount;
};

struct CLikeSourceEmitter::IRDeclaratorInfo
{
    enum class Flavor
    {
        Simple,
        Ptr,
        Array,
    };

    Flavor flavor;
    IRDeclaratorInfo* next;
    union
    {
        String const* name;
        IRInst* elementCount;
    };
};



struct CLikeSourceEmitter::ComputeEmitActionsContext
{
    IRInst*             moduleInst;
    HashSet<IRInst*>    openInsts;
    Dictionary<IRInst*, EmitAction::Level> mapInstToLevel;
    List<EmitAction>*   actions;
};

/* !!!!!!!!!!!!!!!!!!!!!!!!!!!! CLikeSourceEmitter !!!!!!!!!!!!!!!!!!!!!!!!!! */

/* static */CLikeSourceEmitter::SourceStyle CLikeSourceEmitter::getSourceStyle(CodeGenTarget target)
{
    switch (target)
    {
        default:
        case CodeGenTarget::Unknown:
        case CodeGenTarget::None:
        {
            return SourceStyle::Unknown;
        }
        case CodeGenTarget::GLSL:
        case CodeGenTarget::GLSL_Vulkan:
        case CodeGenTarget::GLSL_Vulkan_OneDesc:
        {
            return SourceStyle::GLSL;
        }
        case CodeGenTarget::HLSL:
        {
            return SourceStyle::HLSL;
        }
        case CodeGenTarget::SPIRV:
        case CodeGenTarget::SPIRVAssembly:
        case CodeGenTarget::DXBytecode:
        case CodeGenTarget::DXBytecodeAssembly:
        case CodeGenTarget::DXIL:
        case CodeGenTarget::DXILAssembly:
        {
            return SourceStyle::Unknown;
        }
        case CodeGenTarget::CSource:
        {
            return SourceStyle::C;
        }
        case CodeGenTarget::CPPSource:
        {
            return SourceStyle::CPP;
        }
    }
}

CLikeSourceEmitter::CLikeSourceEmitter(const Desc& desc)
{
    m_writer = desc.sourceWriter;
    m_sourceStyle = getSourceStyle(desc.target);
    SLANG_ASSERT(m_sourceStyle != SourceStyle::Unknown);

    m_target = desc.target;

    m_compileRequest = desc.compileRequest;
    m_entryPoint = desc.entryPoint;
    m_effectiveProfile = desc.effectiveProfile;
    
    m_entryPointLayout = desc.entryPointLayout;
    
    m_programLayout = desc.programLayout;
    m_globalStructLayout = desc.globalStructLayout;
}

//
// Types
//

void CLikeSourceEmitter::emitDeclarator(EDeclarator* declarator)
{
    if (!declarator) return;

    m_writer->emit(" ");

    switch (declarator->flavor)
    {
    case EDeclarator::Flavor::name:
        m_writer->emitName(declarator->name, declarator->loc);
        break;

    case EDeclarator::Flavor::Array:
        emitDeclarator(declarator->next);
        m_writer->emit("[");
        if(auto elementCount = declarator->elementCount)
        {
            emitVal(elementCount, getInfo(EmitOp::General));
        }
        m_writer->emit("]");
        break;

    case EDeclarator::Flavor::UnsizedArray:
        emitDeclarator(declarator->next);
        m_writer->emit("[]");
        break;

    default:
        SLANG_DIAGNOSE_UNEXPECTED(getSink(), SourceLoc(), "unknown declarator flavor");
        break;
    }
}

void CLikeSourceEmitter::emitSimpleType(IRType* type)
{
    emitSimpleTypeImpl(type);
}

/* static */ UnownedStringSlice CLikeSourceEmitter::getDefaultBuiltinTypeName(IROp op)
{
    switch (op)
    {
        case kIROp_VoidType:    return UnownedStringSlice("void");      
        case kIROp_BoolType:    return UnownedStringSlice("bool");      

        case kIROp_Int8Type:    return UnownedStringSlice("int8_t");    
        case kIROp_Int16Type:   return UnownedStringSlice("int16_t");   
        case kIROp_IntType:     return UnownedStringSlice("int");       
        case kIROp_Int64Type:   return UnownedStringSlice("int64_t");   

        case kIROp_UInt8Type:   return UnownedStringSlice("uint8_t");   
        case kIROp_UInt16Type:  return UnownedStringSlice("uint16_t");  
        case kIROp_UIntType:    return UnownedStringSlice("uint");     
        case kIROp_UInt64Type:  return UnownedStringSlice("uint64_t"); 

        case kIROp_HalfType:    return UnownedStringSlice("half");     

        case kIROp_FloatType:   return UnownedStringSlice("float");    
        case kIROp_DoubleType:  return UnownedStringSlice("double");   
        default:                return UnownedStringSlice();
    }
}

void CLikeSourceEmitter::_emitArrayType(IRArrayType* arrayType, EDeclarator* declarator)
{
    EDeclarator arrayDeclarator;
    arrayDeclarator.flavor = EDeclarator::Flavor::Array;
    arrayDeclarator.next = declarator;
    arrayDeclarator.elementCount = arrayType->getElementCount();

    _emitType(arrayType->getElementType(), &arrayDeclarator);
}

void CLikeSourceEmitter::_emitUnsizedArrayType(IRUnsizedArrayType* arrayType, EDeclarator* declarator)
{
    EDeclarator arrayDeclarator;
    arrayDeclarator.flavor = EDeclarator::Flavor::UnsizedArray;
    arrayDeclarator.next = declarator;

    _emitType(arrayType->getElementType(), &arrayDeclarator);
}

void CLikeSourceEmitter::_emitType(IRType* type, EDeclarator* declarator)
{
    switch (type->op)
    {
    default:
        emitSimpleType(type);
        emitDeclarator(declarator);
        break;

    case kIROp_RateQualifiedType:
        {
            auto rateQualifiedType = cast<IRRateQualifiedType>(type);
            _emitType(rateQualifiedType->getValueType(), declarator);
        }
        break;

    case kIROp_ArrayType:
        _emitArrayType(cast<IRArrayType>(type), declarator);
        break;

    case kIROp_UnsizedArrayType:
        _emitUnsizedArrayType(cast<IRUnsizedArrayType>(type), declarator);
        break;
    }

}

void CLikeSourceEmitter::emitType(
    IRType*             type,
    SourceLoc const&    typeLoc,
    Name*               name,
    SourceLoc const&    nameLoc)
{
    m_writer->advanceToSourceLocation(typeLoc);

    EDeclarator nameDeclarator;
    nameDeclarator.flavor = EDeclarator::Flavor::name;
    nameDeclarator.name = name;
    nameDeclarator.loc = nameLoc;
    _emitType(type, &nameDeclarator);
}

void CLikeSourceEmitter::emitType(IRType* type, Name* name)
{
    emitType(type, SourceLoc(), name, SourceLoc());
}

void CLikeSourceEmitter::emitType(IRType* type, const String& name)
{
    // HACK: the rest of the code wants a `Name`,
    // so we'll create one for a bit...
    Name tempName;
    tempName.text = name;

    emitType(type, SourceLoc(), &tempName, SourceLoc());
}


void CLikeSourceEmitter::emitType(IRType* type)
{
    _emitType(type, nullptr);
}

//
// Expressions
//

bool CLikeSourceEmitter::maybeEmitParens(EmitOpInfo& outerPrec, const EmitOpInfo& prec)
{
    bool needParens = (prec.leftPrecedence <= outerPrec.leftPrecedence)
        || (prec.rightPrecedence <= outerPrec.rightPrecedence);

    if (needParens)
    {
        m_writer->emit("(");

        outerPrec = getInfo(EmitOp::None);
    }
    return needParens;
}

void CLikeSourceEmitter::maybeCloseParens(bool needClose)
{
    if(needClose) m_writer->emit(")");
}

bool CLikeSourceEmitter::isTargetIntrinsicModifierApplicable(const String& targetName)
{
    switch(getSourceStyle())
    {
    default:
        SLANG_DIAGNOSE_UNEXPECTED(getSink(), SourceLoc(), "unhandled code generation target");
        return false;

    case SourceStyle::C:    return targetName == "c";
    case SourceStyle::CPP:  return targetName == "cpp";
    case SourceStyle::GLSL: return targetName == "glsl";
    case SourceStyle::HLSL: return targetName == "hlsl";
    }
}

void CLikeSourceEmitter::emitType(IRType* type, Name* name, SourceLoc const& nameLoc)
{
    emitType(
        type,
        SourceLoc(),
        name,
        nameLoc);
}

void CLikeSourceEmitter::emitType(IRType* type, NameLoc const& nameAndLoc)
{
    emitType(type, nameAndLoc.name, nameAndLoc.loc);
}

bool CLikeSourceEmitter::isTargetIntrinsicModifierApplicable(IRTargetIntrinsicDecoration* decoration)
{
    auto targetName = String(decoration->getTargetName());

    // If no target name was specified, then the modifier implicitly
    // applies to all targets.
    if(targetName.getLength() == 0)
        return true;

    return isTargetIntrinsicModifierApplicable(targetName);
}

void CLikeSourceEmitter::emitStringLiteral(String const& value)
{
    m_writer->emit("\"");
    for (auto c : value)
    {
        // TODO: This needs a more complete implementation,
        // especially if we want to support Unicode.

        char buffer[] = { c, 0 };
        switch (c)
        {
        default:
            m_writer->emit(buffer);
            break;

        case '\"': m_writer->emit("\\\"");
        case '\'': m_writer->emit("\\\'");
        case '\\': m_writer->emit("\\\\");
        case '\n': m_writer->emit("\\n");
        case '\r': m_writer->emit("\\r");
        case '\t': m_writer->emit("\\t");
        }
    }
    m_writer->emit("\"");
}

void CLikeSourceEmitter::setSampleRateFlag()
{
    m_entryPointLayout->flags |= EntryPointLayout::Flag::usesAnySampleRateInput;
}

void CLikeSourceEmitter::doSampleRateInputCheck(Name* name)
{
    // TODO(JS): This doesn't appear to be called from anywhere!!
    auto text = getText(name);
    if (text == "gl_SampleID")
    {
        setSampleRateFlag();
    }
}

void CLikeSourceEmitter::emitVal(IRInst* val, EmitOpInfo const& outerPrec)
{
    if(auto type = as<IRType>(val))
    {
        emitType(type);
    }
    else
    {
        emitInstExpr(val, IREmitMode::Default, outerPrec);
    }
}

UInt CLikeSourceEmitter::getBindingOffset(EmitVarChain* chain, LayoutResourceKind kind)
{
    UInt offset = 0;
    for(auto cc = chain; cc; cc = cc->next)
    {
        if(auto resInfo = cc->varLayout->FindResourceInfo(kind))
        {
            offset += resInfo->index;
        }
    }
    return offset;
}

UInt CLikeSourceEmitter::getBindingSpace(EmitVarChain* chain, LayoutResourceKind kind)
{
    UInt space = 0;
    for(auto cc = chain; cc; cc = cc->next)
    {
        auto varLayout = cc->varLayout;
        if(auto resInfo = varLayout->FindResourceInfo(kind))
        {
            space += resInfo->space;
        }
        if(auto resInfo = varLayout->FindResourceInfo(LayoutResourceKind::RegisterSpace))
        {
            space += resInfo->index;
        }
    }
    return space;
}

void CLikeSourceEmitter::emitLayoutDirectives(TargetRequest* targetReq)
{
    // We are going to emit the target-language-specific directives
    // needed to get the default matrix layout to match what was requested
    // for the given target.
    //
    // Note: we do not rely on the defaults for the target language,
    // because a user could take the HLSL/GLSL generated by Slang and pass
    // it to another compiler with non-default options specified on
    // the command line, leading to all kinds of trouble.
    //
    // TODO: We need an approach to "global" layout directives that will work
    // in the presence of multiple modules. If modules A and B were each
    // compiled with different assumptions about how layout is performed,
    // then types/variables defined in those modules should be emitted in
    // a way that is consistent with that layout...

    emitLayoutDirectivesImpl(targetReq);
}

UInt CLikeSourceEmitter::allocateUniqueID()
{
    return m_uniqueIDCounter++;
}

// IR-level emit logic

UInt CLikeSourceEmitter::getID(IRInst* value)
{
    auto& mapIRValueToID = m_mapIRValueToID;

    UInt id = 0;
    if (mapIRValueToID.TryGetValue(value, id))
        return id;

    id = allocateUniqueID();
    mapIRValueToID.Add(value, id);
    return id;
}

String CLikeSourceEmitter::scrubName(const String& name)
{
    // We will use a plain `U` as a dummy character to insert
    // whenever we need to insert things to make a string into
    // valid name.
    //
    char const* dummyChar = "U";

    // Special case a name that is the empty string, just in case.
    if(name.getLength() == 0)
        return dummyChar;

    // Otherwise, we are going to walk over the name byte by byte
    // and write some legal characters to the output as we go.
    StringBuilder sb;

    if(getSourceStyle() == SourceStyle::GLSL)
    {
        // GLSL reserves all names that start with `gl_`,
        // so if we are in danger of collision, then make
        // our name start with a dummy character instead.
        if(name.startsWith("gl_"))
        {
            sb.append(dummyChar);
        }
    }

    // We will also detect user-defined names that
    // might overlap with our convention for mangled names,
    // to avoid an possible collision.
    if(name.startsWith("_S"))
    {
        sb.Append(dummyChar);
    }

    // TODO: This is where we might want to consult
    // a dictionary of reserved words for the chosen target
    //
    //  if(isReservedWord(name)) { sb.Append(dummyChar); }
    //

    // We need to track the previous byte in
    // order to detect consecutive underscores for GLSL.
    int prevChar = -1;

    for(auto c : name)
    {
        // We will treat a dot character just like an underscore
        // for the purposes of producing a scrubbed name, so
        // that we translate `SomeType.someMethod` into
        // `SomeType_someMethod`.
        //
        // By handling this case at the top of this loop, we
        // ensure that a `.`-turned-`_` is handled just like
        // a `_` in the original name, and will be properly
        // scrubbed for GLSL output.
        //
        if(c == '.')
        {
            c = '_';
        }

        if(((c >= 'a') && (c <= 'z'))
            || ((c >= 'A') && (c <= 'Z')))
        {
            // Ordinary ASCII alphabetic characters are assumed
            // to always be okay.
        }
        else if((c >= '0') && (c <= '9'))
        {
            // We don't want to allow a digit as the first
            // byte in a name, since the result wouldn't
            // be a valid identifier in many target languages.
            if(prevChar == -1)
            {
                sb.append(dummyChar);
            }
        }
        else if(c == '_')
        {
            // We will collapse any consecutive sequence of `_`
            // characters into a single one (this means that
            // some names that were unique in the original
            // code might not resolve to unique names after
            // scrubbing, but that was true in general).

            if(prevChar == '_')
            {
                // Skip this underscore, so we don't output
                // more than one in a row.
                continue;
            }
        }
        else
        {
            // If we run into a character that wouldn't normally
            // be allowed in an identifier, we need to translate
            // it into something that *is* valid.
            //
            // Our solution for now will be very clumsy: we will
            // emit `x` and then the hexadecimal version of
            // the byte we were given.
            sb.append("x");
            sb.append(uint32_t((unsigned char) c), 16);

            // We don't want to apply the default handling below,
            // so skip to the top of the loop now.
            prevChar = c;
            continue;
        }

        sb.append(c);
        prevChar = c;
    }

    return sb.ProduceString();
}

String CLikeSourceEmitter::generateName(IRInst* inst)
{
    // If the instruction names something
    // that should be emitted as a target intrinsic,
    // then use that name instead.
    if(auto intrinsicDecoration = findTargetIntrinsicDecoration(inst))
    {
        return String(intrinsicDecoration->getDefinition());
    }

    // If we have a name hint on the instruction, then we will try to use that
    // to provide the actual name in the output code.
    //
    // We need to be careful that the name follows the rules of the target language,
    // so there is a "scrubbing" step that needs to be applied here.
    //
    // We also need to make sure that the name won't collide with other declarations
    // that might have the same name hint applied, so we will still unique
    // them by appending the numeric ID of the instruction.
    //
    // TODO: Find cases where we can drop the suffix safely.
    //
    // TODO: When we start having to handle symbols with external linkage for
    // things like DXIL libraries, we will need to *not* use the friendly
    // names for stuff that should be link-able.
    //
    if(auto nameHintDecoration = inst->findDecoration<IRNameHintDecoration>())
    {
        // The name we output will basically be:
        //
        //      <nameHint>_<uniqueID>
        //
        // Except that we will "scrub" the name hint first,
        // and we will omit the underscore if the (scrubbed)
        // name hint already ends with one.
        //

        String nameHint = nameHintDecoration->getName();
        nameHint = scrubName(nameHint);

        StringBuilder sb;
        sb.append(nameHint);

        // Avoid introducing a double underscore
        if(!nameHint.endsWith("_"))
        {
            sb.append("_");
        }

        String key = sb.ProduceString();
        UInt count = 0;
        m_uniqueNameCounters.TryGetValue(key, count);

        m_uniqueNameCounters[key] = count+1;

        sb.append(Int32(count));
        return sb.ProduceString();
    }

    // If the instruction has a mangled name, then emit using that.
    if(auto linkageDecoration = inst->findDecoration<IRLinkageDecoration>())
    {
        return linkageDecoration->getMangledName();
    }

    // Otherwise fall back to a construct temporary name
    // for the instruction.
    StringBuilder sb;
    sb << "_S";
    sb << Int32(getID(inst));

    return sb.ProduceString();
}

String CLikeSourceEmitter::getName(IRInst* inst)
{
    String name;
    if(!m_mapInstToName.TryGetValue(inst, name))
    {
        name = generateName(inst);
        m_mapInstToName.Add(inst, name);
    }
    return name;
}

void CLikeSourceEmitter::emitDeclarator(IRDeclaratorInfo* declarator)
{
    if(!declarator)
        return;

    switch( declarator->flavor )
    {
    case IRDeclaratorInfo::Flavor::Simple:
        m_writer->emit(" ");
        m_writer->emit(*declarator->name);
        break;

    case IRDeclaratorInfo::Flavor::Ptr:
        m_writer->emit("*");
        emitDeclarator(declarator->next);
        break;

    case IRDeclaratorInfo::Flavor::Array:
        emitDeclarator(declarator->next);
        m_writer->emit("[");
        emitOperand(declarator->elementCount, IREmitMode::Default, getInfo(EmitOp::General));
        m_writer->emit("]");
        break;
    }
}

void CLikeSourceEmitter::emitSimpleValue(IRInst* inst)
{
    switch(inst->op)
    {
    case kIROp_IntLit:
        m_writer->emit(((IRConstant*) inst)->value.intVal);
        break;

    case kIROp_FloatLit:
        m_writer->emit(((IRConstant*) inst)->value.floatVal);
        break;

    case kIROp_BoolLit:
        {
            bool val = ((IRConstant*)inst)->value.intVal != 0;
            m_writer->emit(val ? "true" : "false");
        }
        break;

    default:
        SLANG_UNIMPLEMENTED_X("val case for emit");
        break;
    }

}

bool CLikeSourceEmitter::shouldFoldInstIntoUseSites(IRInst* inst, IREmitMode mode)
{
    // Certain opcodes should never/always be folded in
    switch( inst->op )
    {
    default:
        break;

    // Never fold these in, because they represent declarations
    //
    case kIROp_Var:
    case kIROp_GlobalVar:
    case kIROp_GlobalConstant:
    case kIROp_GlobalParam:
    case kIROp_Param:
    case kIROp_Func:
        return false;

    // Always fold these in, because they are trivial
    //
    case kIROp_IntLit:
    case kIROp_FloatLit:
    case kIROp_BoolLit:
        return true;

    // Always fold these in, because their results
    // cannot be represented in the type system of
    // our current targets.
    //
    // TODO: when we add C/C++ as an optional target,
    // we could consider lowering insts that result
    // in pointers directly.
    //
    case kIROp_FieldAddress:
    case kIROp_getElementPtr:
    case kIROp_Specialize:
        return true;
    }

    // Always fold when we are inside a global constant initializer
    if (mode == IREmitMode::GlobalConstant)
        return true;

    switch( inst->op )
    {
    default:
        break;

    // HACK: don't fold these in because we currently lower
    // them to initializer lists, which aren't allowed in
    // general expression contexts.
    //
    // Note: we are doing this check *after* the check for `GlobalConstant`
    // mode, because otherwise we'd fail to emit initializer lists in
    // the main place where we want/need them.
    //
    case kIROp_makeStruct:
    case kIROp_makeArray:
        return false;

    }

    // Instructions with specific result *types* will usually
    // want to be folded in, because they aren't allowed as types
    // for temporary variables.
    auto type = inst->getDataType();

    // Unwrap any layers of array-ness from the type, so that
    // we can look at the underlying data type, in case we
    // should *never* expose a value of that type
    while (auto arrayType = as<IRArrayTypeBase>(type))
    {
        type = arrayType->getElementType();
    }

    // Don't allow temporaries of pointer types to be created.
    if(as<IRPtrTypeBase>(type))
    {
        return true;
    }

    // First we check for uniform parameter groups,
    // because a `cbuffer` or GLSL `uniform` block
    // does not have a first-class type that we can
    // pass around.
    //
    // TODO: We need to ensure that type legalization
    // cleans up cases where we use a parameter group
    // or parameter block type as a function parameter...
    //
    if(as<IRUniformParameterGroupType>(type))
    {
        // TODO: we need to be careful here, because
        // HLSL shader model 6 allows these as explicit
        // types.
        return true;
    }
    //
    // The stream-output and patch types need to be handled
    // too, because they are not really first class (especially
    // not in GLSL, but they also seem to confuse the HLSL
    // compiler when they get used as temporaries).
    //
    else if (as<IRHLSLStreamOutputType>(type))
    {
        return true;
    }
    else if (as<IRHLSLPatchType>(type))
    {
        return true;
    }

    // GLSL doesn't allow texture/resource types to
    // be used as first-class values, so we need
    // to fold them into their use sites in all cases
    if (getSourceStyle() == SourceStyle::GLSL)
    {
        if(as<IRResourceTypeBase>(type))
        {
            return true;
        }
        else if(as<IRHLSLStructuredBufferTypeBase>(type))
        {
            return true;
        }
        else if(as<IRUntypedBufferResourceType>(type))
        {
            return true;
        }
        else if(as<IRSamplerStateTypeBase>(type))
        {
            return true;
        }
    }

    // If the instruction is at global scope, then it might represent
    // a constant (e.g., the value of an enum case).
    //
    if(as<IRModuleInst>(inst->getParent()))
    {
        if(!inst->mightHaveSideEffects())
            return true;
    }

    // Having dealt with all of the cases where we *must* fold things
    // above, we can now deal with the more general cases where we
    // *should not* fold things.

    // Don't fold something with no users:
    if(!inst->hasUses())
        return false;

    // Don't fold something that has multiple users:
    if(inst->hasMoreThanOneUse())
        return false;

    // Don't fold something that might have side effects:
    if(inst->mightHaveSideEffects())
        return false;

    // Don't fold instructions that are marked `[precise]`.
    // This could in principle be extended to any other
    // decorations that affect the semantics of an instruction
    // in ways that require a temporary to be introduced.
    //
    if(inst->findDecoration<IRPreciseDecoration>())
        return false;

    // Okay, at this point we know our instruction must have a single use.
    auto use = inst->firstUse;
    SLANG_ASSERT(use);
    SLANG_ASSERT(!use->nextUse);

    auto user = use->getUser();

    // We'd like to figure out if it is safe to fold our instruction into `user`

    // First, let's make sure they are in the same block/parent:
    if(inst->getParent() != user->getParent())
        return false;

    // Now let's look at all the instructions between this instruction
    // and the user. If any of them might have side effects, then lets
    // bail out now.
    for(auto ii = inst->getNextInst(); ii != user; ii = ii->getNextInst())
    {
        if(!ii)
        {
            // We somehow reached the end of the block without finding
            // the user, which doesn't make sense if uses dominate
            // defs. Let's just play it safe and bail out.
            return false;
        }

        if(ii->mightHaveSideEffects())
            return false;
    }

    // Okay, if we reach this point then the user comes later in
    // the same block, and there are no instructions with side
    // effects in between, so it seems safe to fold things in.
    return true;
}

void CLikeSourceEmitter::emitOperand(IRInst* inst, IREmitMode mode, EmitOpInfo const&  outerPrec)
{
    if( shouldFoldInstIntoUseSites(inst, mode) )
    {
        emitInstExpr(inst, mode, outerPrec);
        return;
    }

    switch(inst->op)
    {
    case 0: // nothing yet
    default:
        m_writer->emit(getName(inst));
        break;
    }
}

void CLikeSourceEmitter::emitArgs(IRInst* inst, IREmitMode mode)
{
    UInt argCount = inst->getOperandCount();
    IRUse* args = inst->getOperands();

    m_writer->emit("(");
    for(UInt aa = 0; aa < argCount; ++aa)
    {
        if(aa != 0) m_writer->emit(", ");
        emitOperand(args[aa].get(), mode, getInfo(EmitOp::General));
    }
    m_writer->emit(")");
}

void CLikeSourceEmitter::emitRateQualifiers(IRInst* value)
{
    if (IRRate* rate = value->getRate())
    {
        emitRateQualifiersImpl(rate);
    }
}

void CLikeSourceEmitter::emitInstResultDecl(IRInst* inst)
{
    auto type = inst->getDataType();
    if(!type)
        return;

    if (as<IRVoidType>(type))
        return;

    emitTempModifiers(inst);

    emitRateQualifiers(inst);

    emitType(type, getName(inst));
    m_writer->emit(" = ");
}

IRTargetIntrinsicDecoration* CLikeSourceEmitter::findTargetIntrinsicDecoration(IRInst* inst)
{
    for(auto dd : inst->getDecorations())
    {
        if (dd->op != kIROp_TargetIntrinsicDecoration)
            continue;

        auto targetIntrinsic = (IRTargetIntrinsicDecoration*)dd;
        if (isTargetIntrinsicModifierApplicable(targetIntrinsic))
            return targetIntrinsic;
    }

    return nullptr;
}

/* static */bool CLikeSourceEmitter::isOrdinaryName(String const& name)
{
    char const* cursor = name.begin();
    char const* end = name.end();

    while(cursor != end)
    {
        int c = *cursor++;
        if( (c >= 'a') && (c <= 'z') ) continue;
        if( (c >= 'A') && (c <= 'Z') ) continue;
        if( c == '_' ) continue;

        return false;
    }
    return true;
}

void CLikeSourceEmitter::emitTargetIntrinsicCallExpr(
    IRCall*                         inst,
    IRFunc*                         /* func */,
    IRTargetIntrinsicDecoration*    targetIntrinsic,
    IREmitMode                      mode,
    EmitOpInfo const&                  inOuterPrec)
{
    auto outerPrec = inOuterPrec;

    IRUse* args = inst->getOperands();
    Index argCount = inst->getOperandCount();

    // First operand was the function to be called
    args++;
    argCount--;

    auto name = String(targetIntrinsic->getDefinition());

    if(isOrdinaryName(name))
    {
        // Simple case: it is just an ordinary name, so we call it like a builtin.
        auto prec = getInfo(EmitOp::Postfix);
        bool needClose = maybeEmitParens(outerPrec, prec);

        m_writer->emit(name);
        m_writer->emit("(");
        for (Index aa = 0; aa < argCount; ++aa)
        {
            if (aa != 0) m_writer->emit(", ");
            emitOperand(args[aa].get(), mode, getInfo(EmitOp::General));
        }
        m_writer->emit(")");

        maybeCloseParens(needClose);
        return;
    }
    else
    {
        int openParenCount = 0;

        const auto returnType = inst->getDataType();

        // If it returns void -> then we don't need parenthesis 
        if (as<IRVoidType>(returnType) == nullptr)
        {
            m_writer->emit("(");
            openParenCount++;
        }

        // General case: we are going to emit some more complex text.

        char const* cursor = name.begin();
        char const* end = name.end();
        while(cursor != end)
        {
            char c = *cursor++;
            if( c != '$' )
            {
                // Not an escape sequence
                m_writer->emitRawTextSpan(&c, &c+1);
                continue;
            }

            SLANG_RELEASE_ASSERT(cursor != end);

            char d = *cursor++;

            switch (d)
            {
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                {
                    // Simple case: emit one of the direct arguments to the call
                    Index argIndex = d - '0';
                    SLANG_RELEASE_ASSERT((0 <= argIndex) && (argIndex < argCount));
                    m_writer->emit("(");
                    emitOperand(args[argIndex].get(), mode, getInfo(EmitOp::General));
                    m_writer->emit(")");
                }
                break;

            case 'p':
                {
                    // If we are calling a D3D texturing operation in the form t.Foo(s, ...),
                    // then this form will pair up the t and s arguments as needed for a GLSL
                    // texturing operation.
                    SLANG_RELEASE_ASSERT(argCount >= 2);

                    auto textureArg = args[0].get();
                    auto samplerArg = args[1].get();

                    if (auto baseTextureType = as<IRTextureType>(textureArg->getDataType()))
                    {
                        emitTextureOrTextureSamplerTypeImpl(baseTextureType, "sampler");

                        if (auto samplerType = as<IRSamplerStateTypeBase>(samplerArg->getDataType()))
                        {
                            if (as<IRSamplerComparisonStateType>(samplerType))
                            {
                                m_writer->emit("Shadow");
                            }
                        }

                        m_writer->emit("(");
                        emitOperand(textureArg, mode, getInfo(EmitOp::General));
                        m_writer->emit(",");
                        emitOperand(samplerArg, mode, getInfo(EmitOp::General));
                        m_writer->emit(")");
                    }
                    else
                    {
                        SLANG_UNEXPECTED("bad format in intrinsic definition");
                    }
                }
                break;

            case 'c':
                {
                    // When doing texture access in glsl the result may need to be cast.
                    // In particular if the underlying texture is 'half' based, glsl only accesses (read/write)
                    // as float. So we need to cast to a half type on output.
                    // When storing into a texture it is still the case the value written must be half - but
                    // we don't need to do any casting there as half is coerced to float without a problem.
                    SLANG_RELEASE_ASSERT(argCount >= 1);
                        
                    auto textureArg = args[0].get();
                    if (auto baseTextureType = as<IRTextureType>(textureArg->getDataType()))
                    {
                        auto elementType = baseTextureType->getElementType();
                        IRBasicType* underlyingType = nullptr;
                        if (auto basicType = as<IRBasicType>(elementType))
                        {
                            underlyingType = basicType;
                        }
                        else if (auto vectorType = as<IRVectorType>(elementType))
                        {
                            underlyingType = as<IRBasicType>(vectorType->getElementType());
                        }

                        // We only need to output a cast if the underlying type is half.
                        if (underlyingType && underlyingType->op == kIROp_HalfType)
                        {
                            emitSimpleType(elementType);
                            m_writer->emit("(");
                            openParenCount++;
                        }
                    }    
                }
                break;

            case 'z':
                {
                    // If we are calling a D3D texturing operation in the form t.Foo(s, ...),
                    // where `t` is a `Texture*<T>`, then this is the step where we try to
                    // properly swizzle the output of the equivalent GLSL call into the right
                    // shape.
                    SLANG_RELEASE_ASSERT(argCount >= 1);

                    auto textureArg = args[0].get();
                    if (auto baseTextureType = as<IRTextureType>(textureArg->getDataType()))
                    {
                        auto elementType = baseTextureType->getElementType();
                        if (auto basicType = as<IRBasicType>(elementType))
                        {
                            // A scalar result is expected
                            m_writer->emit(".x");
                        }
                        else if (auto vectorType = as<IRVectorType>(elementType))
                        {
                            // A vector result is expected
                            auto elementCount = GetIntVal(vectorType->getElementCount());

                            if (elementCount < 4)
                            {
                                char const* swiz[] = { "", ".x", ".xy", ".xyz", "" };
                                m_writer->emit(swiz[elementCount]);
                            }
                        }
                        else
                        {
                            // What other cases are possible?
                        }
                    }
                    else
                    {
                        SLANG_UNEXPECTED("bad format in intrinsic definition");
                    }
                }
                break;

            case 'N':
                {
                    // Extract the element count from a vector argument so that
                    // we can use it in the constructed expression.

                    SLANG_RELEASE_ASSERT(*cursor >= '0' && *cursor <= '9');
                    Index argIndex = (*cursor++) - '0';
                    SLANG_RELEASE_ASSERT(argCount > argIndex);

                    auto vectorArg = args[argIndex].get();
                    if (auto vectorType = as<IRVectorType>(vectorArg->getDataType()))
                    {
                        auto elementCount = GetIntVal(vectorType->getElementCount());
                        m_writer->emit(elementCount);
                    }
                    else
                    {
                        SLANG_UNEXPECTED("bad format in intrinsic definition");
                    }
                }
                break;

            case 'V':
                {
                    // Take an argument of some scalar/vector type and pad
                    // it out to a 4-vector with the same element type
                    // (this is the inverse of `$z`).
                    //
                    SLANG_RELEASE_ASSERT(*cursor >= '0' && *cursor <= '9');
                    Index argIndex = (*cursor++) - '0';
                    SLANG_RELEASE_ASSERT(argCount > argIndex);

                    auto arg = args[argIndex].get();
                    IRIntegerValue elementCount = 1;
                    IRType* elementType = arg->getDataType();
                    if (auto vectorType = as<IRVectorType>(elementType))
                    {
                        elementCount = GetIntVal(vectorType->getElementCount());
                        elementType = vectorType->getElementType();
                    }

                    if(elementCount == 4)
                    {
                        // In the simple case, the operand is already a 4-vector,
                        // so we can just emit it as-is.
                        emitOperand(arg, mode, getInfo(EmitOp::General));
                    }
                    else
                    {
                        // Otherwise, we need to construct a 4-vector from the
                        // value we have, padding it out with zero elements as
                        // needed.
                        //
                        emitVectorTypeName(elementType, 4);
                        m_writer->emit("(");
                        emitOperand(arg, mode, getInfo(EmitOp::General));
                        for(IRIntegerValue ii = elementCount; ii < 4; ++ii)
                        {
                            m_writer->emit(", ");
                            if(getSourceStyle() == SourceStyle::GLSL)
                            {
                                emitSimpleType(elementType);
                                m_writer->emit("(0)");
                            }
                            else
                            {
                                m_writer->emit("0");
                            }
                        }
                        m_writer->emit(")");
                    }
                }
                break;

            case 'a':
                {
                    // We have an operation that needs to lower to either
                    // `atomic*` or `imageAtomic*` for GLSL, depending on
                    // whether its first operand is a subscript into an
                    // array. This `$a` is the first `a` in `atomic`,
                    // so we will replace it accordingly.
                    //
                    // TODO: This distinction should be made earlier,
                    // with the front-end picking the right overload
                    // based on the "address space" of the argument.

                    Index argIndex = 0;
                    SLANG_RELEASE_ASSERT(argCount > argIndex);

                    auto arg = args[argIndex].get();
                    if(arg->op == kIROp_ImageSubscript)
                    {
                        m_writer->emit("imageA");
                    }
                    else
                    {
                        m_writer->emit("a");
                    }
                }
                break;

            case 'A':
                {
                    // We have an operand that represents the destination
                    // of an atomic operation in GLSL, and it should
                    // be lowered based on whether it is an ordinary l-value,
                    // or an image subscript. In the image subscript case
                    // this operand will turn into multiple arguments
                    // to the `imageAtomic*` function.
                    //

                    Index argIndex = 0;
                    SLANG_RELEASE_ASSERT(argCount > argIndex);

                    auto arg = args[argIndex].get();
                    if(arg->op == kIROp_ImageSubscript)
                    {
                        if(getSourceStyle() == SourceStyle::GLSL)
                        {
                            // TODO: we don't handle the multisample
                            // case correctly here, where the last
                            // component of the image coordinate needs
                            // to be broken out into its own argument.
                            //
                            m_writer->emit("(");
                            emitOperand(arg->getOperand(0), mode, getInfo(EmitOp::General));
                            m_writer->emit("), ");

                            // The coordinate argument will have been computed
                            // as a `vector<uint, N>` because that is how the
                            // HLSL image subscript operations are defined.
                            // In contrast, the GLSL `imageAtomic*` operations
                            // expect `vector<int, N>` coordinates, so we
                            // will hackily insert the conversion here as
                            // part of the intrinsic op.
                            //
                            auto coords = arg->getOperand(1);
                            auto coordsType = coords->getDataType();

                            auto coordsVecType = as<IRVectorType>(coordsType);
                            IRIntegerValue elementCount = 1;
                            if(coordsVecType)
                            {
                                coordsType = coordsVecType->getElementType();
                                elementCount = GetIntVal(coordsVecType->getElementCount());
                            }

                            SLANG_ASSERT(coordsType->op == kIROp_UIntType);

                            if (elementCount > 1)
                            {
                                m_writer->emit("ivec");
                                m_writer->emit(elementCount);
                            }
                            else
                            {
                                m_writer->emit("int");
                            }

                            m_writer->emit("(");
                            emitOperand(arg->getOperand(1), mode, getInfo(EmitOp::General));
                            m_writer->emit(")");
                        }
                        else
                        {
                            m_writer->emit("(");
                            emitOperand(arg, mode, getInfo(EmitOp::General));
                            m_writer->emit(")");
                        }
                    }
                    else
                    {
                        m_writer->emit("(");
                        emitOperand(arg, mode, getInfo(EmitOp::General));
                        m_writer->emit(")");
                    }
                }
                break;

            // We will use the `$X` case as a prefix for
            // special logic needed when cross-compiling ray-tracing
            // shaders.
            case 'X':
                {
                    SLANG_RELEASE_ASSERT(*cursor);
                    switch(*cursor++)
                    {
                    case 'P':
                        {
                            // The `$XP` case handles looking up
                            // the associated `location` for a variable
                            // used as the argument ray payload at a
                            // trace call site.

                            Index argIndex = 0;
                            SLANG_RELEASE_ASSERT(argCount > argIndex);
                            auto arg = args[argIndex].get();
                            auto argLoad = as<IRLoad>(arg);
                            SLANG_RELEASE_ASSERT(argLoad);
                            auto argVar = argLoad->getOperand(0);
                            m_writer->emit(getRayPayloadLocation(argVar));
                        }
                        break;

                    case 'C':
                        {
                            // The `$XC` case handles looking up
                            // the associated `location` for a variable
                            // used as the argument callable payload at a
                            // call site.

                        Index argIndex = 0;
                            SLANG_RELEASE_ASSERT(argCount > argIndex);
                            auto arg = args[argIndex].get();
                            auto argLoad = as<IRLoad>(arg);
                            SLANG_RELEASE_ASSERT(argLoad);
                            auto argVar = argLoad->getOperand(0);
                            m_writer->emit(getCallablePayloadLocation(argVar));
                        }
                        break;

                    case 'T':
                        {
                            // The `$XT` case handles selecting between
                            // the `gl_HitTNV` and `gl_RayTmaxNV` builtins,
                            // based on what stage we are using:
                            switch( m_entryPoint->getStage() )
                            {
                            default:
                                m_writer->emit("gl_RayTmaxNV");
                                break;

                            case Stage::AnyHit:
                            case Stage::ClosestHit:
                                m_writer->emit("gl_HitTNV");
                                break;
                            }
                        }
                        break;

                    default:
                        SLANG_RELEASE_ASSERT(false);
                        break;
                    }
                }
                break;

            default:
                SLANG_UNEXPECTED("bad format in intrinsic definition");
                break;
            }
        }

        // Close any remaining open parens
        for (; openParenCount > 0; --openParenCount)
        {
            m_writer->emit(")");
        }
    }
}

void CLikeSourceEmitter::emitIntrinsicCallExpr(
    IRCall*         inst,
    IRFunc*         func,
    IREmitMode      mode,
    EmitOpInfo const&  inOuterPrec)
{
    auto outerPrec = inOuterPrec;
    bool needClose = false;

    // For a call with N arguments, the instruction will
    // have N+1 operands. We will start consuming operands
    // starting at the index 1.
    UInt operandCount = inst->getOperandCount();
    UInt argCount = operandCount - 1;
    UInt operandIndex = 1;


    //
    if (auto targetIntrinsicDecoration = findTargetIntrinsicDecoration(func))
    {
        emitTargetIntrinsicCallExpr(
            inst,
            func,
            targetIntrinsicDecoration,
            mode,
            outerPrec);
        return;
    }

    // Our current strategy for dealing with intrinsic
    // calls is to "un-mangle" the mangled name, in
    // order to figure out what the user was originally
    // calling. This is a bit messy, and there might
    // be better strategies (including just stuffing
    // a pointer to the original decl onto the callee).

    // If the intrinsic the user is calling is a generic,
    // then the mangled name will have been set on the
    // outer-most generic, and not on the leaf value
    // (which is `func` above), so we need to walk
    // upwards to find it.
    //
    IRInst* valueForName = func;
    for(;;)
    {
        auto parentBlock = as<IRBlock>(valueForName->parent);
        if(!parentBlock)
            break;

        auto parentGeneric = as<IRGeneric>(parentBlock->parent);
        if(!parentGeneric)
            break;

        valueForName = parentGeneric;
    }

    // If we reach this point, we are assuming that the value
    // has some kind of linkage, and thus a mangled name.
    //
    auto linkageDecoration = valueForName->findDecoration<IRLinkageDecoration>();
    SLANG_ASSERT(linkageDecoration);
    auto mangledName = String(linkageDecoration->getMangledName());


    // We will use the `MangledLexer` to
    // help us split the original name into its pieces.
    MangledLexer lexer(mangledName);
    
    // We'll read through the qualified name of the
    // symbol (e.g., `Texture2D<T>.Sample`) and then
    // only keep the last segment of the name (e.g.,
    // the `Sample` part).
    auto name = lexer.readSimpleName();

    // We will special-case some names here, that
    // represent callable declarations that aren't
    // ordinary functions, and thus may use different
    // syntax.
    if(name == "operator[]")
    {
        // The user is invoking a built-in subscript operator

        auto prec = getInfo(EmitOp::Postfix);
        needClose = maybeEmitParens(outerPrec, prec);

        emitOperand(inst->getOperand(operandIndex++), mode, leftSide(outerPrec, prec));
        m_writer->emit("[");
        emitOperand(inst->getOperand(operandIndex++), mode, getInfo(EmitOp::General));
        m_writer->emit("]");

        if(operandIndex < operandCount)
        {
            m_writer->emit(" = ");
            emitOperand(inst->getOperand(operandIndex++), mode, getInfo(EmitOp::General));
        }

        maybeCloseParens(needClose);
        return;
    }

    auto prec = getInfo(EmitOp::Postfix);
    needClose = maybeEmitParens(outerPrec, prec);

    // The mangled function name currently records
    // the number of explicit parameters, and thus
    // doesn't include the implicit `this` parameter.
    // We can compare the argument and parameter counts
    // to figure out whether we have a member function call.
    UInt paramCount = lexer.readParamCount();

    if(argCount != paramCount)
    {
        // Looks like a member function call
        emitOperand(inst->getOperand(operandIndex), mode, leftSide(outerPrec, prec));
        m_writer->emit(".");
        operandIndex++;
    }
    // fixing issue #602 for GLSL sign function: https://github.com/shader-slang/slang/issues/602
    bool glslSignFix = getSourceStyle() == SourceStyle::GLSL && name == "sign";
    if (glslSignFix)
    {
        if (auto vectorType = as<IRVectorType>(inst->getDataType()))
        {
            m_writer->emit("ivec");
            m_writer->emit(as<IRConstant>(vectorType->getElementCount())->value.intVal);
            m_writer->emit("(");
        }
        else if (auto scalarType = as<IRBasicType>(inst->getDataType()))
        {
            m_writer->emit("int(");
        }
        else
            glslSignFix = false;
    }
    m_writer->emit(name);
    m_writer->emit("(");
    bool first = true;
    for(; operandIndex < operandCount; ++operandIndex )
    {
        if(!first) m_writer->emit(", ");
        emitOperand(inst->getOperand(operandIndex), mode, getInfo(EmitOp::General));
        first = false;
    }
    m_writer->emit(")");
    if (glslSignFix)
        m_writer->emit(")");
    maybeCloseParens(needClose);
}

void CLikeSourceEmitter::emitCallExpr(IRCall* inst, IREmitMode mode, EmitOpInfo outerPrec)
{
    auto funcValue = inst->getOperand(0);

    // Does this function declare any requirements.
    handleCallExprDecorationsImpl(funcValue);

    // We want to detect any call to an intrinsic operation,
    // that we can emit it directly without mangling, etc.
    if(auto irFunc = asTargetIntrinsic(funcValue))
    {
        emitIntrinsicCallExpr(inst, irFunc, mode, outerPrec);
    }
    else
    {
        auto prec = getInfo(EmitOp::Postfix);
        bool needClose = maybeEmitParens(outerPrec, prec);

        emitOperand(funcValue, mode, leftSide(outerPrec, prec));
        m_writer->emit("(");
        UInt argCount = inst->getOperandCount();
        for( UInt aa = 1; aa < argCount; ++aa )
        {
            auto operand = inst->getOperand(aa);
            if (as<IRVoidType>(operand->getDataType()))
                continue;
            if(aa != 1) m_writer->emit(", ");
            emitOperand(inst->getOperand(aa), mode, getInfo(EmitOp::General));
        }
        m_writer->emit(")");

        maybeCloseParens(needClose);
    }
}
    
void CLikeSourceEmitter::emitInstExpr(IRInst* inst, IREmitMode mode, const EmitOpInfo& inOuterPrec)
{
    // Try target specific impl first
    if (tryEmitInstExprImpl(inst, mode, inOuterPrec))
    {
        return;
    }

    EmitOpInfo outerPrec = inOuterPrec;
    bool needClose = false;
    switch(inst->op)
    {
    case kIROp_IntLit:
    case kIROp_FloatLit:
    case kIROp_BoolLit:
        emitSimpleValue(inst);
        break;

    case kIROp_Construct:
    case kIROp_makeVector:
    case kIROp_MakeMatrix:
        // Simple constructor call
        emitType(inst->getDataType());
        emitArgs(inst, mode);
        break;

    case kIROp_constructVectorFromScalar:
    {
        // Simple constructor call
        auto prec = getInfo(EmitOp::Prefix);
        needClose = maybeEmitParens(outerPrec, prec);

        m_writer->emit("(");
        emitType(inst->getDataType());
        m_writer->emit(")");

        emitOperand(inst->getOperand(0), mode, rightSide(outerPrec,prec));
        break;
    }
    case kIROp_FieldExtract:
    {
        // Extract field from aggregate
        IRFieldExtract* fieldExtract = (IRFieldExtract*) inst;

        auto prec = getInfo(EmitOp::Postfix);
        needClose = maybeEmitParens(outerPrec, prec);

        auto base = fieldExtract->getBase();
        emitOperand(base, mode, leftSide(outerPrec, prec));
        m_writer->emit(".");
        if(getSourceStyle() == SourceStyle::GLSL
            && as<IRUniformParameterGroupType>(base->getDataType()))
        {
            m_writer->emit("_data.");
        }
        m_writer->emit(getName(fieldExtract->getField()));
        break;
    }
    case kIROp_FieldAddress:
    {
        // Extract field "address" from aggregate

        IRFieldAddress* ii = (IRFieldAddress*) inst;

        auto prec = getInfo(EmitOp::Postfix);
        needClose = maybeEmitParens(outerPrec, prec);

        auto base = ii->getBase();
        emitOperand(base, mode, leftSide(outerPrec, prec));
        m_writer->emit(".");
        if(getSourceStyle() == SourceStyle::GLSL
            && as<IRUniformParameterGroupType>(base->getDataType()))
        {
            m_writer->emit("_data.");
        }
        m_writer->emit(getName(ii->getField()));
        break;
    }

    // Comparisons
    case kIROp_Eql:
    case kIROp_Neq:
    case kIROp_Greater:
    case kIROp_Less:
    case kIROp_Geq:
    case kIROp_Leq:
    {
        const auto emitOp = getEmitOpForOp(inst->op);

        auto prec = getInfo(emitOp);
        needClose = maybeEmitParens(outerPrec, prec);

        emitOperand(inst->getOperand(0), mode, leftSide(outerPrec, prec));
        m_writer->emit(" ");
        m_writer->emit(prec.op);
        m_writer->emit(" ");
        emitOperand(inst->getOperand(1), mode, rightSide(outerPrec, prec));          
        break;
    }

    // Binary ops
    case kIROp_Add:
    case kIROp_Sub:
    case kIROp_Div:
    case kIROp_Mod:
    case kIROp_Lsh:
    case kIROp_Rsh:
    case kIROp_BitXor:
    case kIROp_BitOr:
    case kIROp_BitAnd:
    case kIROp_And:
    case kIROp_Or:
    case kIROp_Mul:
    {
        const auto emitOp = getEmitOpForOp(inst->op);
        const auto info = getInfo(emitOp);

        needClose = maybeEmitParens(outerPrec, info);
        emitOperand(inst->getOperand(0), mode, leftSide(outerPrec, info));    
        m_writer->emit(" ");
        m_writer->emit(info.op);
        m_writer->emit(" ");                                                                  
        emitOperand(inst->getOperand(1), mode, rightSide(outerPrec, info));   
        break;
    }
    // Unary
    case kIROp_Not:
    case kIROp_Neg:
    case kIROp_BitNot:
    {        
        IRInst* operand = inst->getOperand(0);

        const auto emitOp = getEmitOpForOp(inst->op);
        const auto prec = getInfo(emitOp);

        needClose = maybeEmitParens(outerPrec, prec);

        // If it's a BitNot, but the data type is bool special case to !
        if (emitOp == EmitOp::BitNot && as<IRBoolType>(inst->getDataType()))
        {
            m_writer->emit("!");
        }
        else
        {
            m_writer->emit(prec.op);
        }
        
        emitOperand(operand, mode, rightSide(prec, outerPrec));
        break;
    }
    case kIROp_Load:
        {
            auto base = inst->getOperand(0);
            emitOperand(base, mode, outerPrec);
            if(getSourceStyle() == SourceStyle::GLSL
                && as<IRUniformParameterGroupType>(base->getDataType()))
            {
                m_writer->emit("._data");
            }
        }
        break;

    case kIROp_Store:
        {
            auto prec = getInfo(EmitOp::Assign);
            needClose = maybeEmitParens(outerPrec, prec);

            emitOperand(inst->getOperand(0), mode, leftSide(outerPrec, prec));
            m_writer->emit(" = ");
            emitOperand(inst->getOperand(1), mode, rightSide(prec, outerPrec));
        }
        break;

    case kIROp_Call:
        {
            emitCallExpr((IRCall*)inst, mode, outerPrec);
        }
        break;

    case kIROp_GroupMemoryBarrierWithGroupSync:
        m_writer->emit("GroupMemoryBarrierWithGroupSync()");
        break;

    case kIROp_getElement:
    case kIROp_getElementPtr:
    case kIROp_ImageSubscript:
        // HACK: deal with translation of GLSL geometry shader input arrays.
        if(auto decoration = inst->getOperand(0)->findDecoration<IRGLSLOuterArrayDecoration>())
        {
            auto prec = getInfo(EmitOp::Postfix);
            needClose = maybeEmitParens(outerPrec, prec);

            m_writer->emit(decoration->getOuterArrayName());
            m_writer->emit("[");
            emitOperand(inst->getOperand(1), mode, getInfo(EmitOp::General));
            m_writer->emit("].");
            emitOperand(inst->getOperand(0), mode, rightSide(prec, outerPrec));
            break;
        }
        else
        {
            auto prec = getInfo(EmitOp::Postfix);
            needClose = maybeEmitParens(outerPrec, prec);

            emitOperand( inst->getOperand(0), mode, leftSide(outerPrec, prec));
            m_writer->emit("[");
            emitOperand(inst->getOperand(1), mode, getInfo(EmitOp::General));
            m_writer->emit("]");
        }
        break;

    case kIROp_Mul_Vector_Matrix:
    case kIROp_Mul_Matrix_Vector:
    case kIROp_Mul_Matrix_Matrix:
        // Default impl
        m_writer->emit("mul(");
        emitOperand(inst->getOperand(0), mode, getInfo(EmitOp::General));
        m_writer->emit(", ");
        emitOperand(inst->getOperand(1), mode, getInfo(EmitOp::General));
        m_writer->emit(")");
        break;

    case kIROp_swizzle:
        {
            auto prec = getInfo(EmitOp::Postfix);
            needClose = maybeEmitParens(outerPrec, prec);

            auto ii = (IRSwizzle*)inst;
            emitOperand(ii->getBase(), mode, leftSide(outerPrec, prec));
            m_writer->emit(".");
            const Index elementCount = Index(ii->getElementCount());
            for (Index ee = 0; ee < elementCount; ++ee)
            {
                IRInst* irElementIndex = ii->getElementIndex(ee);
                SLANG_RELEASE_ASSERT(irElementIndex->op == kIROp_IntLit);
                IRConstant* irConst = (IRConstant*)irElementIndex;

                UInt elementIndex = (UInt)irConst->value.intVal;
                SLANG_RELEASE_ASSERT(elementIndex < 4);

                char const* kComponents[] = { "x", "y", "z", "w" };
                m_writer->emit(kComponents[elementIndex]);
            }
        }
        break;

    case kIROp_Specialize:
        {
            emitOperand(inst->getOperand(0), mode, outerPrec);
        }
        break;

    case kIROp_Select:
        {
            
            auto prec = getInfo(EmitOp::Conditional);
            needClose = maybeEmitParens(outerPrec, prec);

            emitOperand(inst->getOperand(0), mode, leftSide(outerPrec, prec));
            m_writer->emit(" ? ");
            emitOperand(inst->getOperand(1), mode, prec);
            m_writer->emit(" : ");
            emitOperand(inst->getOperand(2), mode, rightSide(prec, outerPrec));
        }
        break;

    case kIROp_Param:
        m_writer->emit(getName(inst));
        break;

    case kIROp_makeArray:
    case kIROp_makeStruct:
        {
            // TODO: initializer-list syntax may not always
            // be appropriate, depending on the context
            // of the expression.

            m_writer->emit("{ ");
            UInt argCount = inst->getOperandCount();
            for (UInt aa = 0; aa < argCount; ++aa)
            {
                if (aa != 0) m_writer->emit(", ");
                emitOperand(inst->getOperand(aa), mode, getInfo(EmitOp::General));
            }
            m_writer->emit(" }");
        }
        break;

    case kIROp_BitCast:
        {
            // TODO: we can simplify the logic for arbitrary bitcasts
            // by always bitcasting the source to a `uint*` type (if it
            // isn't already) and then bitcasting that to the destination
            // type (if it isn't already `uint*`.
            //
            // For now we are assuming the source type is *already*
            // a `uint*` type of the appropriate size.
            //
            //  auto fromType = extractBaseType(inst->getOperand(0)->getDataType());
         
            m_writer->emit("(");
            emitOperand(inst->getOperand(0), mode, getInfo(EmitOp::General));
            m_writer->emit(")");
        }
        break;

    default:
        m_writer->emit("/* unhandled */");
        break;
    }
    maybeCloseParens(needClose);
}

BaseType CLikeSourceEmitter::extractBaseType(IRType* inType)
{
    auto type = inType;
    for(;;)
    {
        if(auto irBaseType = as<IRBasicType>(type))
        {
            return irBaseType->getBaseType();
        }
        else if(auto vecType = as<IRVectorType>(type))
        {
            type = vecType->getElementType();
            continue;
        }
        else
        {
            return BaseType::Void;
        }
    }
}

void CLikeSourceEmitter::emitInst(IRInst* inst, IREmitMode mode)
{
    try
    {
        _emitInst(inst, mode);
    }
    // Don't emit any context message for an explicit `AbortCompilationException`
    // because it should only happen when an error is already emitted.
    catch(AbortCompilationException&) { throw; }
    catch(...)
    {
        noteInternalErrorLoc(inst->sourceLoc);
        throw;
    }
}

void CLikeSourceEmitter::_emitInst(IRInst* inst, IREmitMode mode)
{
    if (shouldFoldInstIntoUseSites(inst, mode))
    {
        return;
    }

    m_writer->advanceToSourceLocation(inst->sourceLoc);

    switch(inst->op)
    {
    default:
        emitInstResultDecl(inst);
        emitInstExpr(inst, mode, getInfo(EmitOp::General));
        m_writer->emit(";\n");
        break;

    case kIROp_undefined:
        {
            auto type = inst->getDataType();
            emitType(type, getName(inst));
            m_writer->emit(";\n");
        }
        break;

    case kIROp_Var:
        {
            auto ptrType = cast<IRPtrType>(inst->getDataType());
            auto valType = ptrType->getValueType();

            auto name = getName(inst);
            emitRateQualifiers(inst);
            emitType(valType, name);
            m_writer->emit(";\n");
        }
        break;

    case kIROp_Param:
        // Don't emit parameters, since they are declared as part of the function.
        break;

    case kIROp_FieldAddress:
        // skip during code emit, since it should be
        // folded into use site(s)
        break;

    case kIROp_ReturnVoid:
        m_writer->emit("return;\n");
        break;

    case kIROp_ReturnVal:
        m_writer->emit("return ");
        emitOperand(((IRReturnVal*) inst)->getVal(), mode, getInfo(EmitOp::General));
        m_writer->emit(";\n");
        break;

    case kIROp_discard:
        m_writer->emit("discard;\n");
        break;

    case kIROp_swizzleSet:
        {
            auto ii = (IRSwizzleSet*)inst;
            emitInstResultDecl(inst);
            emitOperand(inst->getOperand(0), mode, getInfo(EmitOp::General));
            m_writer->emit(";\n");

            auto subscriptOuter = getInfo(EmitOp::General);
            auto subscriptPrec = getInfo(EmitOp::Postfix);
            bool needCloseSubscript = maybeEmitParens(subscriptOuter, subscriptPrec);

            emitOperand(inst, mode, leftSide(subscriptOuter, subscriptPrec));
            m_writer->emit(".");
            UInt elementCount = ii->getElementCount();
            for (UInt ee = 0; ee < elementCount; ++ee)
            {
                IRInst* irElementIndex = ii->getElementIndex(ee);
                SLANG_RELEASE_ASSERT(irElementIndex->op == kIROp_IntLit);
                IRConstant* irConst = (IRConstant*)irElementIndex;

                UInt elementIndex = (UInt)irConst->value.intVal;
                SLANG_RELEASE_ASSERT(elementIndex < 4);

                char const* kComponents[] = { "x", "y", "z", "w" };
                m_writer->emit(kComponents[elementIndex]);
            }
            maybeCloseParens(needCloseSubscript);

            m_writer->emit(" = ");
            emitOperand(inst->getOperand(1), mode, getInfo(EmitOp::General));
            m_writer->emit(";\n");
        }
        break;

    case kIROp_SwizzledStore:
        {
            auto subscriptOuter = getInfo(EmitOp::General);
            auto subscriptPrec = getInfo(EmitOp::Postfix);
            bool needCloseSubscript = maybeEmitParens(subscriptOuter, subscriptPrec);


            auto ii = cast<IRSwizzledStore>(inst);
            emitOperand(ii->getDest(), mode, leftSide(subscriptOuter, subscriptPrec));
            m_writer->emit(".");
            UInt elementCount = ii->getElementCount();
            for (UInt ee = 0; ee < elementCount; ++ee)
            {
                IRInst* irElementIndex = ii->getElementIndex(ee);
                SLANG_RELEASE_ASSERT(irElementIndex->op == kIROp_IntLit);
                IRConstant* irConst = (IRConstant*)irElementIndex;

                UInt elementIndex = (UInt)irConst->value.intVal;
                SLANG_RELEASE_ASSERT(elementIndex < 4);

                char const* kComponents[] = { "x", "y", "z", "w" };
                m_writer->emit(kComponents[elementIndex]);
            }
            maybeCloseParens(needCloseSubscript);

            m_writer->emit(" = ");
            emitOperand(ii->getSource(), mode, getInfo(EmitOp::General));
            m_writer->emit(";\n");
        }
        break;
    }
}

void CLikeSourceEmitter::emitSemantics(VarLayout* varLayout)
{
    if(varLayout->flags & VarLayoutFlag::HasSemantic)
    {
        m_writer->emit(" : ");
        m_writer->emit(varLayout->semanticName);
        if(varLayout->semanticIndex)
        {
            m_writer->emit(varLayout->semanticIndex);
        }
    }
}

void CLikeSourceEmitter::emitSemantics(IRInst* inst)
{
    emitSemanticsImpl(inst);
}

VarLayout* CLikeSourceEmitter::getVarLayout(IRInst* var)
{
    auto decoration = var->findDecoration<IRLayoutDecoration>();
    if (!decoration)
        return nullptr;

    return (VarLayout*) decoration->getLayout();
}

void CLikeSourceEmitter::emitLayoutSemantics(IRInst* inst, char const* uniformSemanticSpelling)
{
    emitLayoutSemanticsImpl(inst, uniformSemanticSpelling);
}

void CLikeSourceEmitter::emitPhiVarAssignments(UInt argCount, IRUse* args, IRBlock* targetBlock)
{
    UInt argCounter = 0;
    for (auto pp = targetBlock->getFirstParam(); pp; pp = pp->getNextParam())
    {
        UInt argIndex = argCounter++;

        if (argIndex >= argCount)
        {
            SLANG_UNEXPECTED("not enough arguments for branch");
            break;
        }

        IRInst* arg = args[argIndex].get();

        auto outerPrec = getInfo(EmitOp::General);
        auto prec = getInfo(EmitOp::Assign);

        emitOperand(pp, IREmitMode::Default, leftSide(outerPrec, prec));
        m_writer->emit(" = ");
        emitOperand(arg, IREmitMode::Default, rightSide(prec, outerPrec));
        m_writer->emit(";\n");
    }
}

void CLikeSourceEmitter::emitRegion(Region* inRegion)
{
    // We will use a loop so that we can process sequential (simple)
    // regions iteratively rather than recursively.
    // This is effectively an emulation of tail recursion.
    Region* region = inRegion;
    while(region)
    {
        // What flavor of region are we trying to emit?
        switch(region->getFlavor())
        {
        case Region::Flavor::Simple:
            {
                // A simple region consists of a basic block followed
                // by another region.
                //
                auto simpleRegion = (SimpleRegion*) region;

                // We start by outputting all of the non-terminator
                // instructions in the block.
                //
                auto block = simpleRegion->block;
                auto terminator = block->getTerminator();
                for (auto inst = block->getFirstInst(); inst != terminator; inst = inst->getNextInst())
                {
                    emitInst(inst, IREmitMode::Default);
                }

                // Next we have to deal with the terminator instruction
                // itself. In many cases, the terminator will have been
                // turned into a block of its own, but certain cases
                // of terminators are simple enough that we just fold
                // them into the current block.
                //
                m_writer->advanceToSourceLocation(terminator->sourceLoc);
                switch(terminator->op)
                {
                default:
                    // Don't do anything with the terminator, and assume
                    // its behavior has been folded into the next region.
                    break;

                case kIROp_ReturnVal:
                case kIROp_ReturnVoid:
                case kIROp_discard:
                    // For extremely simple terminators, we just handle
                    // them here, so that we don't have to allocate
                    // separate `Region`s for them.
                    emitInst(terminator, IREmitMode::Default);
                    break;

                // We will also handle any unconditional branches
                // here, since they may have arguments to pass
                // to the target block (our encoding of SSA
                // "phi" operations).
                //
                // TODO: A better approach would be to move out of SSA
                // as an IR pass, and introduce explicit variables to
                // replace any "phi nodes." This would avoid possible
                // complications if we ever end up in the bad case where
                // one of the block arguments on a branch is also
                // a parameter of the target block, so that the order
                // of operations is important.
                //
                case kIROp_unconditionalBranch:
                    {
                        auto t = (IRUnconditionalBranch*)terminator;
                        UInt argCount = t->getOperandCount();
                        static const UInt kFixedArgCount = 1;
                        emitPhiVarAssignments(
                            argCount - kFixedArgCount,
                            t->getOperands() + kFixedArgCount,
                            t->getTargetBlock());
                    }
                    break;
                case kIROp_loop:
                    {
                        auto t = (IRLoop*) terminator;
                        UInt argCount = t->getOperandCount();
                        static const UInt kFixedArgCount = 3;
                        emitPhiVarAssignments(
                            argCount - kFixedArgCount,
                            t->getOperands() + kFixedArgCount,
                            t->getTargetBlock());

                    }
                    break;
                }

                // If the terminator required a full region to represent
                // its behavior in a structured form, then we will move
                // along to that region now.
                //
                // We do this iteratively rather than recursively, by
                // jumping back to the top of our loop with a new
                // value for `region`.
                //
                region = simpleRegion->nextRegion;
                continue;
            }

        // Break and continue regions are trivial to handle, as long as we
        // don't need to consider multi-level break/continue (which we
        // don't for now).
        case Region::Flavor::Break:
            m_writer->emit("break;\n");
            break;
        case Region::Flavor::Continue:
            m_writer->emit("continue;\n");
            break;

        case Region::Flavor::If:
            {
                auto ifRegion = (IfRegion*) region;

                // TODO: consider simplifying the code in
                // the case where `ifRegion == null`
                // so that we output `if(!condition) { elseRegion }`
                // instead of the current `if(condition) {} else { elseRegion }`

                m_writer->emit("if(");
                emitOperand(ifRegion->condition, IREmitMode::Default, getInfo(EmitOp::General));
                m_writer->emit(")\n{\n");
                m_writer->indent();
                emitRegion(ifRegion->thenRegion);
                m_writer->dedent();
                m_writer->emit("}\n");

                // Don't emit the `else` region if it would be empty
                //
                if(auto elseRegion = ifRegion->elseRegion)
                {
                    m_writer->emit("else\n{\n");
                    m_writer->indent();
                    emitRegion(elseRegion);
                    m_writer->dedent();
                    m_writer->emit("}\n");
                }

                // Continue with the region after the `if`.
                //
                // TODO: consider just constructing a `SimpleRegion`
                // around an `IfRegion` to handle this sequencing,
                // rather than making `IfRegion` serve as both a
                // conditional and a sequence.
                //
                region = ifRegion->nextRegion;
                continue;
            }
            break;

        case Region::Flavor::Loop:
            {
                auto loopRegion = (LoopRegion*) region;
                auto loopInst = loopRegion->loopInst;

                // If the user applied an explicit decoration to the loop,
                // to control its unrolling behavior, then pass that
                // along in the output code (if the target language
                // supports the semantics of the decoration).
                //
                if (auto loopControlDecoration = loopInst->findDecoration<IRLoopControlDecoration>())
                {
                    switch (loopControlDecoration->getMode())
                    {
                    case kIRLoopControl_Unroll:
                        // Note: loop unrolling control is only available in HLSL, not GLSL
                        if(getSourceStyle() == SourceStyle::HLSL)
                        {
                            m_writer->emit("[unroll]\n");
                        }
                        break;

                    default:
                        break;
                    }
                }

                m_writer->emit("for(;;)\n{\n");
                m_writer->indent();
                emitRegion(loopRegion->body);
                m_writer->dedent();
                m_writer->emit("}\n");

                // Continue with the region after the loop
                region = loopRegion->nextRegion;
                continue;
            }

        case Region::Flavor::Switch:
            {
                auto switchRegion = (SwitchRegion*) region;

                // Emit the start of our statement.
                m_writer->emit("switch(");
                emitOperand(switchRegion->condition, IREmitMode::Default, getInfo(EmitOp::General));
                m_writer->emit(")\n{\n");

                auto defaultCase = switchRegion->defaultCase;
                for(auto currentCase : switchRegion->cases)
                {
                    for(auto caseVal : currentCase->values)
                    {
                        m_writer->emit("case ");
                        emitOperand(caseVal, IREmitMode::Default, getInfo(EmitOp::General));
                        m_writer->emit(":\n");
                    }
                    if(currentCase.Ptr() == defaultCase)
                    {
                        m_writer->emit("default:\n");
                    }

                    m_writer->indent();
                    m_writer->emit("{\n");
                    m_writer->indent();
                    emitRegion(currentCase->body);
                    m_writer->dedent();
                    m_writer->emit("}\n");
                    m_writer->dedent();
                }

                m_writer->emit("}\n");

                // Continue with the region after the `switch`
                region = switchRegion->nextRegion;
                continue;
            }
            break;
        }
        break;
    }
}

void CLikeSourceEmitter::emitRegionTree(RegionTree* regionTree)
{
    emitRegion(regionTree->rootRegion);
}

bool CLikeSourceEmitter::isDefinition(IRFunc* func)
{
    // For now, we use a simple approach: a function is
    // a definition if it has any blocks, and a declaration otherwise.
    return func->getFirstBlock() != nullptr;
}

String CLikeSourceEmitter::getFuncName(IRFunc* func)
{
    if (auto entryPointLayout = asEntryPoint(func))
    {
        // GLSL will always need to use `main` as the
        // name for an entry-point function, but other
        // targets should try to use the original name.
        //
        // TODO: always use `main`, and have any code
        // that wraps this know to use `main` instead
        // of the original entry-point name...
        //
        if (getSourceStyle() != SourceStyle::GLSL)
        {
            return getText(entryPointLayout->entryPoint->getName());
        }

        //

        return "main";
    }
    else
    {
        return getName(func);
    }
}


void CLikeSourceEmitter::emitEntryPointAttributes(IRFunc* irFunc, EntryPointLayout* entryPointLayout)
{
    emitEntryPointAttributesImpl(irFunc, entryPointLayout);
}

void CLikeSourceEmitter::emitPhiVarDecls(IRFunc* func)
{
    // We will skip the first block, since its parameters are
    // the parameters of the whole function.
    auto bb = func->getFirstBlock();
    if (!bb)
        return;
    bb = bb->getNextBlock();

    for (; bb; bb = bb->getNextBlock())
    {
        for (auto pp = bb->getFirstParam(); pp; pp = pp->getNextParam())
        {
            emitTempModifiers(pp);
            emitType(pp->getFullType(), getName(pp));
            m_writer->emit(";\n");
        }
    }
}

void CLikeSourceEmitter::emitFunctionBody(IRGlobalValueWithCode* code)
{
    // Compute a structured region tree that can represent
    // the control flow of our function.
    //
    RefPtr<RegionTree> regionTree = generateRegionTreeForFunc(code, getSink());

    // Now that we've computed the region tree, we have
    // an opportunity to perform some last-minute transformations
    // on the code to make sure it follows our rules.
    //
    // TODO: it would be better to do these transformations earlier,
    // so that we can, e.g., dump the final IR code *before* emission
    // starts, but that gets a bit complicated because we also want
    // to have the region tree available without having to recompute it.
    //
    // For now we are just going to do things the expedient way, but
    // eventually we should allow an IR module to have side-band
    // storage for derived structures like the region tree (and logic
    // for invalidating them when a transformation would break them).
    //
    fixValueScoping(regionTree);

    // Now emit high-level code from that structured region tree.
    //
    emitRegionTree(regionTree);
}

void CLikeSourceEmitter::emitSimpleFuncParamImpl(IRParam* param)
{
    auto paramName = getName(param);
    auto paramType = param->getDataType();

    emitParamType(paramType, paramName);
    emitSemantics(param);
}

void CLikeSourceEmitter::emitSimpleFunc(IRFunc* func)
{
    auto resultType = func->getResultType();

    // Deal with decorations that need
    // to be emitted as attributes
    auto entryPointLayout = asEntryPoint(func);
    if (entryPointLayout)
    {
        emitEntryPointAttributes(func, entryPointLayout);
    }

    auto name = getFuncName(func);

    emitType(resultType, name);

    m_writer->emit("(");
    auto firstParam = func->getFirstParam();
    for( auto pp = firstParam; pp; pp = pp->getNextParam())
    {
        if(pp != firstParam)
            m_writer->emit(", ");

        emitSimpleFuncParamImpl(pp);
    }
    m_writer->emit(")");

    emitSemantics(func);

    // TODO: encode declaration vs. definition
    if(isDefinition(func))
    {
        m_writer->emit("\n{\n");
        m_writer->indent();

        // HACK: forward-declare all the local variables needed for the
        // parameters of non-entry blocks.
        emitPhiVarDecls(func);

        // Need to emit the operations in the blocks of the function
        emitFunctionBody(func);

        m_writer->dedent();
        m_writer->emit("}\n\n");
    }
    else
    {
        m_writer->emit(";\n\n");
    }
}

void CLikeSourceEmitter::emitParamType(IRType* type, String const& name)
{
    // An `out` or `inout` parameter will have been
    // encoded as a parameter of pointer type, so
    // we need to decode that here.
    //
    if( auto outType = as<IROutType>(type))
    {
        m_writer->emit("out ");
        type = outType->getValueType();
    }
    else if( auto inOutType = as<IRInOutType>(type))
    {
        m_writer->emit("inout ");
        type = inOutType->getValueType();
    }
    else if( auto refType = as<IRRefType>(type))
    {
        // Note: There is no HLSL/GLSL equivalent for by-reference parameters,
        // so we don't actually expect to encounter these in user code.
        m_writer->emit("inout ");
        type = inOutType->getValueType();
    }

    emitType(type, name);
}

IRInst* CLikeSourceEmitter::getSpecializedValue(IRSpecialize* specInst)
{
    auto base = specInst->getBase();
    auto baseGeneric = as<IRGeneric>(base);
    if (!baseGeneric)
        return base;

    auto lastBlock = baseGeneric->getLastBlock();
    if (!lastBlock)
        return base;

    auto returnInst = as<IRReturnVal>(lastBlock->getTerminator());
    if (!returnInst)
        return base;

    return returnInst->getVal();
}

void CLikeSourceEmitter::emitFuncDecl(IRFunc* func)
{
    // We don't want to emit declarations for operations
    // that only appear in the IR as stand-ins for built-in
    // operations on that target.
    if (isTargetIntrinsic(func))
        return;

    // Finally, don't emit a declaration for an entry point,
    // because it might need meta-data attributes attached
    // to it, and the HLSL compiler will get upset if the
    // forward declaration doesn't *also* have those
    // attributes.
    if(asEntryPoint(func))
        return;


    // A function declaration doesn't have any IR basic blocks,
    // and as a result it *also* doesn't have the IR `param` instructions,
    // so we need to emit a declaration entirely from the type.

    auto funcType = func->getDataType();
    auto resultType = func->getResultType();

    auto name = getFuncName(func);

    emitType(resultType, name);

    m_writer->emit("(");
    auto paramCount = funcType->getParamCount();
    for(UInt pp = 0; pp < paramCount; ++pp)
    {
        if(pp != 0)
            m_writer->emit(", ");

        String paramName;
        paramName.append("_");
        paramName.append(Int32(pp));
        auto paramType = funcType->getParamType(pp);

        emitParamType(paramType, paramName);
    }
    m_writer->emit(");\n\n");
}

EntryPointLayout* CLikeSourceEmitter::getEntryPointLayout(IRFunc* func)
{
    if( auto layoutDecoration = func->findDecoration<IRLayoutDecoration>() )
    {
        return as<EntryPointLayout>(layoutDecoration->getLayout());
    }
    return nullptr;
}

EntryPointLayout* CLikeSourceEmitter::asEntryPoint(IRFunc* func)
{
    if (auto layoutDecoration = func->findDecoration<IRLayoutDecoration>())
    {
        if (auto entryPointLayout = as<EntryPointLayout>(layoutDecoration->getLayout()))
        {
            return entryPointLayout;
        }
    }

    return nullptr;
}

bool CLikeSourceEmitter::isTargetIntrinsic(IRFunc* func)
{
    // For now we do this in an overly simplistic
    // fashion: we say that *any* function declaration
    // (rather then definition) must be an intrinsic:
    return !isDefinition(func);
}

IRFunc* CLikeSourceEmitter::asTargetIntrinsic(IRInst* value)
{
    if(!value)
        return nullptr;

    while (auto specInst = as<IRSpecialize>(value))
    {
        value = getSpecializedValue(specInst);
    }

    if(value->op != kIROp_Func)
        return nullptr;

    IRFunc* func = (IRFunc*) value;
    if(!isTargetIntrinsic(func))
        return nullptr;

    return func;
}

void CLikeSourceEmitter::emitFunc(IRFunc* func)
{
    if(!isDefinition(func))
    {
        // This is just a function declaration,
        // and so we want to emit it as such.
        // (Or maybe not emit it at all).

        // We do not emit the declaration for
        // functions that appear to be intrinsics/builtins
        // in the target language.
        if (isTargetIntrinsic(func))
            return;

        emitFuncDecl(func);
    }
    else
    {
        // The common case is that what we
        // have is just an ordinary function,
        // and we can emit it as such.
        emitSimpleFunc(func);
    }
}

void CLikeSourceEmitter::emitStruct(IRStructType* structType)
{
    // If the selected `struct` type is actually an intrinsic
    // on our target, then we don't want to emit anything at all.
    if(auto intrinsicDecoration = findTargetIntrinsicDecoration(structType))
    {
        return;
    }

    m_writer->emit("struct ");
    m_writer->emit(getName(structType));
    m_writer->emit("\n{\n");
    m_writer->indent();

    for(auto ff : structType->getFields())
    {
        auto fieldKey = ff->getKey();
        auto fieldType = ff->getFieldType();

        // Filter out fields with `void` type that might
        // have been introduced by legalization.
        if(as<IRVoidType>(fieldType))
            continue;

        // Note: GLSL doesn't support interpolation modifiers on `struct` fields
        if( getSourceStyle() != SourceStyle::GLSL )
        {
            emitInterpolationModifiers(fieldKey, fieldType, nullptr);
        }

        emitType(fieldType, getName(fieldKey));
        emitSemantics(fieldKey);
        m_writer->emit(";\n");
    }

    m_writer->dedent();
    m_writer->emit("};\n\n");
}

void CLikeSourceEmitter::emitInterpolationModifiers(IRInst* varInst, IRType* valueType, VarLayout* layout)
{
    emitInterpolationModifiersImpl(varInst, valueType, layout);
}

UInt CLikeSourceEmitter::getRayPayloadLocation(IRInst* inst)
{
    auto& map = m_mapIRValueToRayPayloadLocation;
    UInt value = 0;
    if(map.TryGetValue(inst, value))
        return value;

    value = map.Count();
    map.Add(inst, value);
    return value;
}

UInt CLikeSourceEmitter::getCallablePayloadLocation(IRInst* inst)
{
    auto& map = m_mapIRValueToCallablePayloadLocation;
    UInt value = 0;
    if(map.TryGetValue(inst, value))
        return value;

    value = map.Count();
    map.Add(inst, value);
    return value;
}

    /// Emit modifiers that should apply even for a declaration of an SSA temporary.
void CLikeSourceEmitter::emitTempModifiers(IRInst* temp)
{
    if(temp->findDecoration<IRPreciseDecoration>())
    {
        m_writer->emit("precise ");
    }
}

void CLikeSourceEmitter::emitVarModifiers(VarLayout* layout, IRInst* varDecl, IRType* varType)
{
    // TODO(JS): We could push all of this onto the target impls, and then not need so many virtual hooks.
    emitVarDecorationsImpl(varDecl);

    emitTempModifiers(varDecl);

    if (!layout)
        return;

    emitMatrixLayoutModifiersImpl(layout);

    // Target specific modifier output
    emitImageFormatModifierImpl(varDecl, varType);

    if(layout->FindResourceInfo(LayoutResourceKind::VaryingInput)
        || layout->FindResourceInfo(LayoutResourceKind::VaryingOutput))
    {
        emitInterpolationModifiers(varDecl, varType, layout);
    }

    // Output target specific qualifiers
    emitLayoutQualifiersImpl(layout);
}

void CLikeSourceEmitter::emitArrayBrackets(IRType* inType)
{
    // A declaration may require zero, one, or
    // more array brackets. When writing out array
    // brackets from left to right, they represent
    // the structure of the type from the "outside"
    // in (that is, if we have a 5-element array of
    // 3-element arrays we should output `[5][3]`),
    // because of C-style declarator rules.
    //
    // This conveniently means that we can print
    // out all the array brackets with a looping
    // rather than a recursive structure.
    //
    // We will peel the input type like an onion,
    // looking at one layer at a time until we
    // reach a non-array type in the middle.
    //
    IRType* type = inType;
    for(;;)
    {
        if(auto arrayType = as<IRArrayType>(type))
        {
            m_writer->emit("[");
            emitVal(arrayType->getElementCount(), getInfo(EmitOp::General));
            m_writer->emit("]");

            // Continue looping on the next layer in.
            //
            type = arrayType->getElementType();
        }
        else if(auto unsizedArrayType = as<IRUnsizedArrayType>(type))
        {
            m_writer->emit("[]");

            // Continue looping on the next layer in.
            //
            type = unsizedArrayType->getElementType();
        }
        else
        {
            // This layer wasn't an array, so we are done.
            //
            return;
        }
    }
}

void CLikeSourceEmitter::emitParameterGroup(IRGlobalParam* varDecl, IRUniformParameterGroupType* type)
{
    emitParameterGroupImpl(varDecl, type);
}

void CLikeSourceEmitter::emitVar(IRVar* varDecl)
{
    auto allocatedType = varDecl->getDataType();
    auto varType = allocatedType->getValueType();
//        auto addressSpace = allocatedType->getAddressSpace();

#if 0
    switch( varType->op )
    {
    case kIROp_ConstantBufferType:
    case kIROp_TextureBufferType:
        emitIRParameterGroup(ctx, varDecl, (IRUniformBufferType*) varType);
        return;

    default:
        break;
    }
#endif

    // Need to emit appropriate modifiers here.

    auto layout = getVarLayout(varDecl);

    emitVarModifiers(layout, varDecl, varType);

#if 0
    switch (addressSpace)
    {
    default:
        break;

    case kIRAddressSpace_GroupShared:
        emit("groupshared ");
        break;
    }
#endif
    emitRateQualifiers(varDecl);

    emitType(varType, getName(varDecl));

    emitSemantics(varDecl);

    emitLayoutSemantics(varDecl);

    m_writer->emit(";\n");
}

void CLikeSourceEmitter::emitGlobalVar(IRGlobalVar* varDecl)
{
    auto allocatedType = varDecl->getDataType();
    auto varType = allocatedType->getValueType();

    String initFuncName;
    if (varDecl->getFirstBlock())
    {
        // A global variable with code means it has an initializer
        // associated with it. Eventually we'd like to emit that
        // initializer directly as an expression here, but for
        // now we'll emit it as a separate function.

        initFuncName = getName(varDecl);
        initFuncName.append("_init");

        m_writer->emit("\n");
        emitType(varType, initFuncName);
        m_writer->emit("()\n{\n");
        m_writer->indent();
        emitFunctionBody(varDecl);
        m_writer->dedent();
        m_writer->emit("}\n");
    }

    // An ordinary global variable won't have a layout
    // associated with it, since it is not a shader
    // parameter.
    //
    SLANG_ASSERT(!getVarLayout(varDecl));
    VarLayout* layout = nullptr;

    // An ordinary global variable (which is not a
    // shader parameter) may need special
    // modifiers to indicate it as such.
    //
    switch (getSourceStyle())
    {
    case SourceStyle::HLSL:
        // HLSL requires the `static` modifier on any
        // global variables; otherwise they are assumed
        // to be uniforms.
        m_writer->emit("static ");
        break;

    default:
        break;
    }

    emitVarModifiers(layout, varDecl, varType);

    emitRateQualifiers(varDecl);
    emitType(varType, getName(varDecl));

    // TODO: These shouldn't be needed for ordinary
    // global variables.
    //
    emitSemantics(varDecl);
    emitLayoutSemantics(varDecl);

    if (varDecl->getFirstBlock())
    {
        m_writer->emit(" = ");
        m_writer->emit(initFuncName);
        m_writer->emit("()");
    }

    m_writer->emit(";\n\n");
}

void CLikeSourceEmitter::emitGlobalParam(IRGlobalParam* varDecl)
{
    auto rawType = varDecl->getDataType();

    auto varType = rawType;
    if( auto outType = as<IROutTypeBase>(varType) )
    {
        varType = outType->getValueType();
    }
    if (as<IRVoidType>(varType))
        return;

    // When a global shader parameter represents a "parameter group"
    // (either a constant buffer or a parameter block with non-resource
    // data in it), we will prefer to emit it as an ordinary `cbuffer`
    // declaration or `uniform` block, even when emitting HLSL for
    // D3D profiles that support the explicit `ConstantBuffer<T>` type.
    //
    // Alternatively, we could make this choice based on profile, and
    // prefer `ConstantBuffer<T>` on profiles that support it and/or when
    // the input code used that syntax.
    //
    if (auto paramBlockType = as<IRUniformParameterGroupType>(varType))
    {
        emitParameterGroup(varDecl, paramBlockType);
        return;
    }

    // Try target specific ways to emit.
    if (tryEmitGlobalParamImpl(varDecl, varType))
    {
        return;
    }

    // Need to emit appropriate modifiers here.

    // We expect/require all shader parameters to
    // have some kind of layout information associated with them.
    //
    auto layout = getVarLayout(varDecl);
    SLANG_ASSERT(layout);

    emitVarModifiers(layout, varDecl, varType);

    emitRateQualifiers(varDecl);
    emitType(varType, getName(varDecl));

    emitSemantics(varDecl);

    emitLayoutSemantics(varDecl);

    // A shader parameter cannot have an initializer,
    // so we do need to consider emitting one here.

    m_writer->emit(";\n\n");
}


void CLikeSourceEmitter::emitGlobalConstantInitializer(IRGlobalConstant* valDecl)
{
    // We expect to see only a single block
    auto block = valDecl->getFirstBlock();
    SLANG_RELEASE_ASSERT(block);
    SLANG_RELEASE_ASSERT(!block->getNextBlock());

    // We expect the terminator to be a `return`
    // instruction with a value.
    auto returnInst = (IRReturnVal*) block->getLastDecorationOrChild();
    SLANG_RELEASE_ASSERT(returnInst->op == kIROp_ReturnVal);

    // We will emit the value in the `GlobalConstant` mode, which
    // more or less says to fold all instructions into their use
    // sites, so that we end up with a single expression tree even
    // in cases that would otherwise trip up our analysis.
    //
    // Note: We are emitting the value as an *operand* here instead
    // of directly calling `emitIRInstExpr` because we need to handle
    // cases where the value might *need* to emit as a named referenced
    // (e.g., when it names another constant directly).
    //
    emitOperand(returnInst->getVal(), IREmitMode::GlobalConstant, getInfo(EmitOp::General));
}

void CLikeSourceEmitter::emitGlobalConstant(IRGlobalConstant* valDecl)
{
    auto valType = valDecl->getDataType();

    if( getSourceStyle() != SourceStyle::GLSL )
    {
        m_writer->emit("static ");
    }
    m_writer->emit("const ");
    emitRateQualifiers(valDecl);
    emitType(valType, getName(valDecl));

    if (valDecl->getFirstBlock())
    {
        // There is an initializer (which we expect for
        // any global constant...).

        m_writer->emit(" = ");

        // We need to emit the entire initializer as
        // a single expression.
        emitGlobalConstantInitializer(valDecl);
    }


    m_writer->emit(";\n");
}

void CLikeSourceEmitter::emitGlobalInst(IRInst* inst)
{
    m_writer->advanceToSourceLocation(inst->sourceLoc);

    switch(inst->op)
    {
    case kIROp_Func:
        emitFunc((IRFunc*) inst);
        break;

    case kIROp_GlobalVar:
        emitGlobalVar((IRGlobalVar*) inst);
        break;

    case kIROp_GlobalParam:
        emitGlobalParam((IRGlobalParam*) inst);
        break;

    case kIROp_GlobalConstant:
        emitGlobalConstant((IRGlobalConstant*) inst);
        break;

    case kIROp_Var:
        emitVar((IRVar*) inst);
        break;

    case kIROp_StructType:
        emitStruct(cast<IRStructType>(inst));
        break;

    default:
        break;
    }
}

void CLikeSourceEmitter::ensureInstOperand(ComputeEmitActionsContext* ctx, IRInst* inst, EmitAction::Level requiredLevel)
{
    if(!inst) return;

    if(inst->getParent() == ctx->moduleInst)
    {
        ensureGlobalInst(ctx, inst, requiredLevel);
    }
}

void CLikeSourceEmitter::ensureInstOperandsRec(ComputeEmitActionsContext* ctx, IRInst* inst)
{
    ensureInstOperand(ctx, inst->getFullType());

    UInt operandCount = inst->operandCount;
    for(UInt ii = 0; ii < operandCount; ++ii)
    {
        // TODO: there are some special cases we can add here,
        // to avoid outputting full definitions in cases that
        // can get by with forward declarations.
        //
        // For example, true pointer types should (in principle)
        // only need the type they point to to be forward-declared.
        // Similarly, a `call` instruction only needs the callee
        // to be forward-declared, etc.

        ensureInstOperand(ctx, inst->getOperand(ii));
    }

    for(auto child : inst->getDecorationsAndChildren())
    {
        ensureInstOperandsRec(ctx, child);
    }
}

void CLikeSourceEmitter::ensureGlobalInst(ComputeEmitActionsContext* ctx, IRInst* inst, EmitAction::Level requiredLevel)
{
    // Skip certain instructions, since they
    // don't affect output.
    switch(inst->op)
    {
    case kIROp_WitnessTable:
    case kIROp_Generic:
        return;

    default:
        break;
    }

    // Have we already processed this instruction?
    EmitAction::Level existingLevel;
    if(ctx->mapInstToLevel.TryGetValue(inst, existingLevel))
    {
        // If we've already emitted it suitably,
        // then don't worry about it.
        if(existingLevel >= requiredLevel)
            return;
    }

    EmitAction action;
    action.level = requiredLevel;
    action.inst = inst;

    if(requiredLevel == EmitAction::Level::Definition)
    {
        if(ctx->openInsts.Contains(inst))
        {
            SLANG_UNEXPECTED("circularity during codegen");
            return;
        }

        ctx->openInsts.Add(inst);

        ensureInstOperandsRec(ctx, inst);

        ctx->openInsts.Remove(inst);
    }

    ctx->mapInstToLevel[inst] = requiredLevel;
    ctx->actions->add(action);
}

void CLikeSourceEmitter::computeEmitActions(IRModule* module, List<EmitAction>& ioActions)
{
    ComputeEmitActionsContext ctx;
    ctx.moduleInst = module->getModuleInst();
    ctx.actions = &ioActions;

    for(auto inst : module->getGlobalInsts())
    {
        if( as<IRType>(inst) )
        {
            // Don't emit a type unless it is actually used.
            continue;
        }

        ensureGlobalInst(&ctx, inst, EmitAction::Level::Definition);
    }
}

void CLikeSourceEmitter::executeEmitActions(List<EmitAction> const& actions)
{
    for(auto action : actions)
    {
        switch(action.level)
        {
        case EmitAction::Level::ForwardDeclaration:
            emitFuncDecl(cast<IRFunc>(action.inst));
            break;

        case EmitAction::Level::Definition:
            emitGlobalInst(action.inst);
            break;
        }
    }
}

void CLikeSourceEmitter::emitModule(IRModule* module)
{
    // The IR will usually come in an order that respects
    // dependencies between global declarations, but this
    // isn't guaranteed, so we need to be careful about
    // the order in which we emit things.

    List<EmitAction> actions;

    computeEmitActions(module, actions);
    executeEmitActions(actions);
}

} // namespace Slang
