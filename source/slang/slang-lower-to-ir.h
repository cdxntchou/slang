// slang-lower-to-ir.h
#ifndef SLANG_LOWER_TO_IR_H_INCLUDED
#define SLANG_LOWER_TO_IR_H_INCLUDED

// The lowering step translates from a (type-checked) AST into
// our intermediate representation, to facilitate further
// optimization and transformation.

#include "../core/slang-basic.h"

#include "slang-compiler.h"
#include "slang-ir.h"

namespace Slang
{
    class EntryPoint;
    class ProgramLayout;
    class TranslationUnitRequest;

    IRModule* generateIRForTranslationUnit(
        TranslationUnitRequest* translationUnit);

    RefPtr<IRModule> generateIRForProgram(
        Session*        session,
        Program*        program,
        DiagnosticSink* sink);
}
#endif
